#include "app.hpp"
#include "image_io.hpp"
#include "parameters.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_stdlib.h>

#ifdef _WIN32
#include <windows.h>
#include <shobjidl.h>
#endif

#include <GLFW/glfw3.h>
#include "image_texture.hpp"
#include "native_frame.hpp"

namespace { NativeFrame native_frame; }

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <random>
#include <system_error>
#include <utility>
#include <condition_variable>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

namespace {
struct Telemetry {
    float cpu = 0.0f, gpu = -1.0f;
    float ram_used_gb = 0.0f, ram_total_gb = 0.0f;
    float vram_used_gb = 0.0f, vram_total_gb = 0.0f;
};

Telemetry sample_telemetry() {
    static Telemetry cached;
    static auto last_update = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now - last_update < std::chrono::milliseconds(500)) return cached;
    last_update = now;

    static ULONGLONG previous_idle = 0, previous_kernel = 0, previous_user = 0;
    FILETIME idle{}, kernel{}, user{};
    if (GetSystemTimes(&idle, &kernel, &user)) {
        const auto as_u64 = [](FILETIME value) { return (static_cast<ULONGLONG>(value.dwHighDateTime) << 32) | value.dwLowDateTime; };
        const ULONGLONG i = as_u64(idle), k = as_u64(kernel), u = as_u64(user);
        const ULONGLONG total = (k - previous_kernel) + (u - previous_user);
        if (previous_kernel && total) cached.cpu = 100.0f * static_cast<float>(total - (i - previous_idle)) / static_cast<float>(total);
        previous_idle = i; previous_kernel = k; previous_user = u;
    }
    MEMORYSTATUSEX memory{sizeof(memory)};
    if (GlobalMemoryStatusEx(&memory)) {
        constexpr float gib = 1024.0f * 1024.0f * 1024.0f;
        cached.ram_total_gb = memory.ullTotalPhys / gib;
        cached.ram_used_gb = (memory.ullTotalPhys - memory.ullAvailPhys) / gib;
    }

    using nvmlReturn_t = int;
    using nvmlDevice_t = void*;
    struct NvmlUtilization { unsigned int gpu, memory; };
    struct NvmlMemory { unsigned long long total, free, used; };
    static HMODULE nvml = LoadLibraryW(L"nvml.dll");
    static bool initialized = false;
    if (nvml) {
        auto init = reinterpret_cast<nvmlReturn_t(*)()>(GetProcAddress(nvml, "nvmlInit_v2"));
        auto get_device = reinterpret_cast<nvmlReturn_t(*)(unsigned int, nvmlDevice_t*)>(GetProcAddress(nvml, "nvmlDeviceGetHandleByIndex_v2"));
        auto get_util = reinterpret_cast<nvmlReturn_t(*)(nvmlDevice_t, NvmlUtilization*)>(GetProcAddress(nvml, "nvmlDeviceGetUtilizationRates"));
        auto get_memory = reinterpret_cast<nvmlReturn_t(*)(nvmlDevice_t, NvmlMemory*)>(GetProcAddress(nvml, "nvmlDeviceGetMemoryInfo"));
        if (!initialized && init) initialized = init() == 0;
        nvmlDevice_t device = nullptr;
        if (initialized && get_device && get_device(0, &device) == 0) {
            NvmlUtilization utilization{};
            NvmlMemory gpu_memory{};
            if (get_util && get_util(device, &utilization) == 0) cached.gpu = static_cast<float>(utilization.gpu);
            if (get_memory && get_memory(device, &gpu_memory) == 0) {
                constexpr float gib = 1024.0f * 1024.0f * 1024.0f;
                cached.vram_used_gb = gpu_memory.used / gib;
                cached.vram_total_gb = gpu_memory.total / gib;
            }
        }
    }
    return cached;
}
Telemetry read_telemetry() {
    struct Sampler {
        std::mutex mutex;
        Telemetry value;
        std::condition_variable_any wake;
        std::jthread worker;
        Sampler():worker([this](std::stop_token stop) {
            while(!stop.stop_requested()) {
                const auto next=sample_telemetry();
                std::unique_lock lock(mutex);value=next;
                wake.wait_for(lock,stop,std::chrono::milliseconds(500),[]{return false;});
            }
        }) {}
    };
    static Sampler sampler;
    std::scoped_lock lock(sampler.mutex);return sampler.value;
}

constexpr std::array<const char*, 17> sampler_labels = {
    "Euler", "Euler a", "Heun", "DPM2", "DPM++ 2S a", "DPM++ 2M",
    "DPM++ 2M v2", "DPM++ 2M SDE", "DPM++ 2M SDE Brownian", "DDIM",
    "LCM", "TCD", "IPNDM", "IPNDM V", "Res Multistep", "Res 2S", "LMS"
};
constexpr std::array<const char*, 17> sampler_values = {
    "euler", "euler_a", "heun", "dpm2", "dpm++2s_a", "dpm++2m",
    "dpm++2mv2", "dpm++2m_sde", "dpm++2m_sde_bt", "ddim_trailing",
    "lcm", "tcd", "ipndm", "ipndm_v", "res_multistep", "res_2s", "lms"
};
constexpr std::array<const char*, 11> scheduler_labels = {
    "Normal / Discrete", "Karras", "Exponential", "AYS", "SGM Uniform",
    "Simple", "Smoothstep", "KL Optimal", "LCM", "Beta", "GITS"
};
constexpr std::array<const char*, 11> scheduler_values = {
    "discrete", "karras", "exponential", "ays", "sgm_uniform",
    "simple", "smoothstep", "kl_optimal", "lcm", "beta", "gits"
};

std::filesystem::path application_directory() {
#ifdef _WIN32
    wchar_t value[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, value, MAX_PATH) != 0)
        return std::filesystem::path(value).parent_path();
#endif
    return std::filesystem::current_path();
}

std::filesystem::path settings_directory() {
    return application_directory() / L"config";
}

void glfw_error_callback(int error, const char* description) {
    std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

std::string path_to_utf8(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
#else
    return path.string();
#endif
}

void format_prompt_lines(std::string& prompt) {
    std::string formatted;
    formatted.reserve(prompt.size() + 16);
    bool after_comma = false;
    for (const char c : prompt) {
        if (after_comma && (c == ' ' || c == '\t' || c == '\r' || c == '\n')) continue;
        if (after_comma) { formatted += '\n'; after_comma = false; }
        formatted += c;
        if (c == ',') after_comma = true;
    }
    prompt = std::move(formatted);
}
}

bool App::initialize() {
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) return false;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    window_ = glfwCreateWindow(1440, 900, "Fox Diffusion", nullptr, nullptr);
    if (!window_) return false;
    glfwSetWindowSizeLimits(window_, 780, 480, GLFW_DONT_CARE, GLFW_DONT_CARE);
    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    const auto settings_root = settings_directory();
    std::error_code directory_error;
    std::filesystem::create_directories(settings_root, directory_error);
    const auto layout = settings_root / L"layout.ini";
    if (!std::filesystem::exists(layout, directory_error)) {
        const auto old_layout = application_directory() / L"layout.ini";
        if (std::filesystem::is_regular_file(old_layout, directory_error))
            ImGui::LoadIniSettingsFromDisk(path_to_utf8(old_layout).c_str());
    }
    imgui_ini_path_ = path_to_utf8(layout);
    io.IniFilename = imgui_ini_path_.c_str();
    apply_theme();

    if (!ImGui_ImplGlfw_InitForOpenGL(window_, true)) return false;
    glfwSetWindowUserPointer(window_,this);
    glfwSetDropCallback(window_,[](GLFWwindow* window,int count,const char** paths){
        if(count<1)return;auto* app=static_cast<App*>(glfwGetWindowUserPointer(window));
        app->img2img_.load_path(std::filesystem::u8path(paths[0]));app->img2img_active_=true;
    });
    if (!ImGui_ImplOpenGL3_Init("#version 330")) return false;


    native_frame.install(window_, [this] { render_frame(); });
    load_settings();
    apply_theme();
    return true;
}

void App::run() {
    double last_interaction = 0;
    double previous_frame_start = glfwGetTime();
    while (!glfwWindowShouldClose(window_)) {
        // VSync paces active interaction; only idle windows use the low-power wait.
        if (backend_.running() || glfwGetTime() - last_interaction < .35)
            glfwPollEvents();
        else {
            const double remaining = 1.0 / idle_fps_ - (glfwGetTime() - previous_frame_start);
            if (remaining > 0) glfwWaitEventsTimeout(remaining);
            else glfwPollEvents();
        }
        previous_frame_start = glfwGetTime();
        render_frame();
        const auto& input = ImGui::GetIO();
        if (input.MouseDelta.x != 0 || input.MouseDelta.y != 0 || input.MouseWheel != 0 ||
            input.MouseDown[0] || input.MouseDown[1] || input.MouseDown[2] || input.WantTextInput || ImGui::IsAnyItemActive())
            last_interaction = glfwGetTime();
    }
}

void App::render_frame() {
    if (native_frame.drawing || glfwGetWindowAttrib(window_, GLFW_ICONIFIED)) return;
    native_frame.drawing = true;
    glfwMakeContextCurrent(window_);
        backend_.poll_snapshot(frame_snapshot_);
        update_preview_texture();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        // Alt belongs to the editor (color picking / subtracting selections),
        // not Dear ImGui's press-and-release menu-layer shortcut.
        const ImGuiID editor_alt_owner = ImHashStr("FoxEditorAlt");
        ImGui::SetKeyOwner(ImGuiKey_LeftAlt, editor_alt_owner);
        ImGui::SetKeyOwner(ImGuiKey_RightAlt, editor_alt_owner);
        ImGui::SetKeyOwner(ImGuiMod_Alt, editor_alt_owner);
        if (GImGui->NavWindowingInputSource == ImGuiInputSource_Keyboard)
            GImGui->NavWindowingToggleLayer = false;

        draw_workspace();
        draw_settings();
        if (show_demo_window_) ImGui::ShowDemoWindow(&show_demo_window_);

        const ImVec4 modal_dim=ImGui::GetStyle().Colors[ImGuiCol_ModalWindowDimBg];
        if(settings_open_)ImGui::GetStyle().Colors[ImGuiCol_ModalWindowDimBg]=ImVec4(0,0,0,0);
        ImGui::Render();
        ImGui::GetStyle().Colors[ImGuiCol_ModalWindowDimBg]=modal_dim;
        int display_w = 0, display_h = 0;
        glfwGetFramebufferSize(window_, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.035f, 0.039f, 0.052f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            GLFWwindow* backup = glfwGetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            glfwMakeContextCurrent(backup);
        }
        glfwSwapBuffers(window_);
    native_frame.drawing = false;
}

void App::shutdown() {
    save_settings();
    ImGui::SaveIniSettingsToDisk(imgui_ini_path_.c_str());
    backend_.cancel();
    img2img_.shutdown();
    clear_image_texture(preview_texture_);
    clear_image_texture(settings_blur_);
    native_frame.remove();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    if (window_) glfwDestroyWindow(window_);
    glfwTerminate();
}

void App::draw_workspace() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    constexpr ImGuiWindowFlags root_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 9));
    ImGui::Begin("Fox Diffusion Workspace", nullptr, root_flags);
    ImGui::PopStyleVar();
    ImGui::PopStyleVar(2);
    draw_menu_bar();
    workspace_mode_=img2img_active_;
    const ImGuiID txt_id=ImGui::GetID("FoxTextLayoutV3"), img_id=ImGui::GetID("FoxImageLayoutV3");
    const ImGuiID dockspace_id=workspace_mode_?img_id:txt_id;
    // Both docking trees stay alive; panels have separate identities per mode.
    const ImGuiID inactive_id=workspace_mode_?txt_id:img_id;
    if(ImGui::DockBuilderGetNode(inactive_id))
        ImGui::DockSpace(inactive_id,ImVec2(0,0),ImGuiDockNodeFlags_KeepAliveOnly);
    if (reset_workspace_layout_ || ImGui::DockBuilderGetNode(dockspace_id) == nullptr) {
        reset_workspace_layout_ = false;
        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, ImVec2(viewport->WorkSize.x, viewport->WorkSize.y - 48.0f));

        ImGuiID preview_dock = dockspace_id;
        ImGuiID left_dock = ImGui::DockBuilderSplitNode(
            preview_dock, ImGuiDir_Left, 0.34f, nullptr, &preview_dock);
        ImGuiID models_dock = ImGui::DockBuilderSplitNode(
            left_dock, ImGuiDir_Down, 0.32f, nullptr, &left_dock);

        ImGui::DockBuilderDockWindow(panel_title("Generation").c_str(), left_dock);
        ImGui::DockBuilderDockWindow(panel_title("Models").c_str(), models_dock);
        if(img2img_active_){
            ImGuiID result_dock=ImGui::DockBuilderSplitNode(preview_dock,ImGuiDir_Down,.40f,nullptr,&preview_dock);
            ImGui::DockBuilderDockWindow(panel_title("Result").c_str(),result_dock);
            ImGui::DockBuilderDockWindow("Img2img editor",preview_dock);
        }else ImGui::DockBuilderDockWindow(panel_title("Result").c_str(), preview_dock);
        ImGui::DockBuilderFinish(dockspace_id);
    }
    ImGui::DockSpace(dockspace_id, ImVec2(0, -28), ImGuiDockNodeFlags_PassthruCentralNode);
    draw_status_bar();
    ImGui::End();

    draw_prompt_panel();
    draw_preview_panel();
    draw_models_panel();
    if(workspace_mode_){img2img_.draw(width_,height_);
        if(!img2img_.pending_parameters.empty()){
            GenerationRequest r;r.sampling_method=sampler_values[sampler_];r.scheduler=scheduler_values[scheduler_];apply_parameters(r,img2img_.pending_parameters);
            positive_prompt_=r.positive_prompt;negative_prompt_=r.negative_prompt;steps_=r.steps;cfg_scale_=r.cfg_scale;seed_=r.seed;random_seed_=r.random_seed;width_=r.width;height_=r.height;img2img_.options.strength=r.img2img.strength;
            for(size_t i=0;i<sampler_values.size();++i)if(r.sampling_method==sampler_values[i])sampler_=int(i);
            for(size_t i=0;i<scheduler_values.size();++i)if(r.scheduler==scheduler_values[i])scheduler_=int(i);
            img2img_.pending_parameters.clear();
        }
    }
}

void App::draw_caption_buttons() {
    const float menu_end = ImGui::GetCursorScreenPos().x;
    const auto* viewport = ImGui::GetMainViewport();
    const float height = ImGui::GetCurrentWindow()->MenuBarHeight;
    const float button_width = height * 1.45f;
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowWidth();
    const float left = right - button_width * 3;
    native_frame.drag = {static_cast<LONG>(menu_end - viewport->Pos.x), 0,
        static_cast<LONG>(left - viewport->Pos.x), static_cast<LONG>(height)};
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 title_size = ImGui::CalcTextSize("Fox Diffusion");
    if (left - menu_end > title_size.x + 32)
        draw->AddText(ImVec2(menu_end + (left-menu_end-title_size.x)*.5f,
            ImGui::GetWindowPos().y + (height-title_size.y)*.5f),
            ImGui::GetColorU32(ImGuiCol_TextDisabled), "Fox Diffusion");
    const char* ids[] = {"##minimize", "##maximize", "##close"};
    for (int i = 0; i < 3; ++i) {
        const ImVec2 p(left + i * button_width, ImGui::GetWindowPos().y);
        ImGui::SetCursorScreenPos(p);
        const bool pressed = ImGui::InvisibleButton(ids[i], ImVec2(button_width, height));
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) draw->AddRectFilled(p, ImVec2(p.x + button_width, p.y + height),
            i == 2 ? IM_COL32(210, 38, 48, 255) : ImGui::GetColorU32(ImGuiCol_ButtonHovered));
        const ImU32 color = hovered && i == 2 ? IM_COL32_WHITE : ImGui::GetColorU32(ImGuiCol_Text);
        const ImVec2 center(p.x + button_width * .5f, p.y + height * .5f);
        const float r = 5;
        if (i == 0) draw->AddLine(ImVec2(center.x-r,center.y), ImVec2(center.x+r,center.y),color);
        else if (i == 1) {
            const bool maximized = IsZoomed(native_frame.hwnd);
            if (maximized) draw->AddRect(ImVec2(center.x-r+3,center.y-r-2),ImVec2(center.x+r+2,center.y+r-3),color);
            draw->AddRectFilled(ImVec2(center.x-r,center.y-r),ImVec2(center.x+r,center.y+r),
                ImGui::GetColorU32(hovered ? ImGuiCol_ButtonHovered : ImGuiCol_MenuBarBg));
            draw->AddRect(ImVec2(center.x-r,center.y-r),ImVec2(center.x+r,center.y+r),color);
        } else {
            draw->AddLine(ImVec2(center.x-r,center.y-r),ImVec2(center.x+r,center.y+r),color);
            draw->AddLine(ImVec2(center.x+r,center.y-r),ImVec2(center.x-r,center.y+r),color);
        }
        if (pressed) {
            const WPARAM command = i == 0 ? SC_MINIMIZE : i == 2 ? SC_CLOSE :
                IsZoomed(native_frame.hwnd) ? SC_RESTORE : SC_MAXIMIZE;
            PostMessageW(native_frame.hwnd, WM_SYSCOMMAND, command, 0);
        }
    }
}

void App::draw_menu_bar() {
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Choose model folder...")) choose_model_folder();
        const auto& snapshot = frame_snapshot_;
        if (ImGui::MenuItem("Save image as...", "Ctrl+Shift+S", false, !snapshot.rgba.empty())) {
            save_current_image();
        }
        if (ImGui::MenuItem("Settings...")) { settings_open_=true; settings_capture_=true; }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Alt+F4")) glfwSetWindowShouldClose(window_, GLFW_TRUE);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Reset current mode layout")) reset_workspace_layout_ = true;
        ImGui::MenuItem("ImGui demo", nullptr, &show_demo_window_);
        ImGui::EndMenu();
    }
    if (ImGui::Button("Preview")) ImGui::OpenPopup("Preview settings");
    if (ImGui::BeginPopup("Preview settings")) {
        ImGui::TextDisabled("LIVE PREVIEW");
        ImGui::SetNextItemWidth(230);
        const char* modes[] = {"Off (fastest)", "Projected (fast)", "Tiny Autoencoder", "Full VAE (slow)"};
        ImGui::Combo("Mode", &preview_mode_, modes, IM_ARRAYSIZE(modes));
        ImGui::SetNextItemWidth(230);
        ImGui::SliderInt("Every N steps", &preview_interval_, 1, 20);
        ImGui::Checkbox("Denoised preview", &preview_denoised_);
        ImGui::Checkbox("Noisy preview", &preview_noisy_);
        ImGui::Separator();
        if (preview_mode_ == 0) ImGui::TextDisabled("No callback overhead.");
        else if (preview_mode_ == 1) ImGui::TextDisabled("Recommended: low overhead, no extra model.");
        else if (preview_mode_ == 2) ImGui::TextDisabled("Requires a compatible TAESD model.");
        else ImGui::TextDisabled("Highest preview cost; decodes through the VAE.");
        if (ImGui::Button("Close", ImVec2(-1, 0))) {
            save_settings();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if(ImGui::RadioButton("txt2img",!img2img_active_))img2img_active_=false;
    if(ImGui::RadioButton("img2img",img2img_active_))img2img_active_=true;
    draw_caption_buttons();
    ImGui::EndMenuBar();
}


void App::draw_output_canvas(const std::vector<std::uint8_t>& pixels, int width, int height, ImVec2 size) {
    ImGui::BeginChild("outputCanvas", size, ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 available = ImGui::GetContentRegionAvail();
    available.x = std::max(1.f, available.x); available.y = std::max(1.f, available.y);
    ImGui::InvisibleButton("##output_interaction", available,
        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(pos, ImVec2(pos.x + available.x, pos.y + available.y), IM_COL32(16, 18, 24, 255));
    if (pixels.empty() || width <= 0 || height <= 0) {
        draw->AddText(ImVec2(pos.x + 16, pos.y + 16), IM_COL32(145, 150, 165, 255), "Your result will appear here");
        ImGui::EndChild(); return;
    }
    if (width != output_view_width_ || height != output_view_height_) {
        output_view_width_ = width; output_view_height_ = height;
        output_zoom_ = 1; output_pan_x_ = output_pan_y_ = 0;
    }
    const float fit = std::min(available.x / width, available.y / height);
    if (output_actual_size_) {
        output_zoom_ = 1.f / fit; output_pan_x_ = output_pan_y_ = 0; output_actual_size_ = false;
    }
    auto& io = ImGui::GetIO();
    if (hovered && io.MouseWheel != 0) {
        const float old = output_zoom_;
        output_zoom_ = std::clamp(output_zoom_ * std::pow(1.2f, io.MouseWheel), .1f, std::max(32.f, 8.f / fit));
        const float ratio = output_zoom_ / old;
        output_pan_x_ = (io.MousePos.x - pos.x - available.x * .5f) * (1 - ratio) + output_pan_x_ * ratio;
        output_pan_y_ = (io.MousePos.y - pos.y - available.y * .5f) * (1 - ratio) + output_pan_y_ * ratio;
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        output_zoom_ = 1; output_pan_x_ = output_pan_y_ = 0;
    } else if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Left) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle))) {
        output_pan_x_ += io.MouseDelta.x; output_pan_y_ += io.MouseDelta.y;
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }
    const float scale = fit * output_zoom_;
    const ImVec2 origin(pos.x + (available.x - width * scale) * .5f + output_pan_x_,
                        pos.y + (available.y - height * scale) * .5f + output_pan_y_);
    draw->PushClipRect(pos, ImVec2(pos.x + available.x, pos.y + available.y), true);
    draw_image_texture(preview_texture_, draw, origin, scale);
    draw->PopClipRect();
    char view_label[96];
    std::snprintf(view_label, sizeof(view_label), "%d x %d  |  %.0f%%", width, height, scale * 100);
    const ImVec2 label_size = ImGui::CalcTextSize(view_label);
    const ImVec2 label_pos(pos.x + 7, pos.y + available.y - label_size.y - 7);
    draw->AddRectFilled(ImVec2(label_pos.x - 4, label_pos.y - 3),
                        ImVec2(label_pos.x + label_size.x + 4, label_pos.y + label_size.y + 3), IM_COL32(16,18,24,205), 0);
    draw->AddText(label_pos, IM_COL32(195,200,215,255), view_label);
    ImGui::EndChild();
}

void App::draw_preview_panel() {
    ImGui::Begin(panel_title("Result").c_str(), nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const auto& state = frame_snapshot_;
    const auto* pixels = &state.rgba;
    int image_width = state.width, image_height = state.height;
    if (!state.running && !state.outputs.empty()) {
        if (gallery_index_ >= 0 && !gallery_image_.empty()) {
            pixels = &gallery_image_.pixels; image_width = gallery_image_.w; image_height = gallery_image_.h;
        }
    }
    auto accept = [&]() {
        Raster image; image.w = image_width; image.h = image_height; image.pixels = *pixels;
        img2img_.accept_result(std::move(image)); img2img_active_ = true;
    };
    auto toolbar = [&](bool enlarged) {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(5, 3));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(5, 4));
        if (ImGui::Button("Fit")) { output_zoom_ = 1; output_pan_x_ = output_pan_y_ = 0; }
        ImGui::SetItemTooltip("Fit image. Wheel: zoom at pointer. Drag: pan. Double-click: fit.");
        ImGui::SameLine();
        if (ImGui::Button("1:1")) output_actual_size_ = true;
        if (!enlarged) {
            ImGui::SameLine();
            if (ImGui::Button("Enlarge")) ImGui::OpenPopup("Inspect result");
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(pixels->empty() || state.running);
        if (ImGui::Button("Apply to editor")) accept();
        ImGui::SetItemTooltip("Apply to the editor and clear the mask. Undo restores the previous image and mask.");
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("More")) ImGui::OpenPopup("output_actions");
        if (ImGui::BeginPopup("output_actions")) {
            if (ImGui::MenuItem("Save as PNG / JPEG...", nullptr, false, !pixels->empty())) save_current_image();
            ImGui::TextDisabled("Wheel: zoom | Drag: pan | Double-click: fit");
            ImGui::EndPopup();
        }
        if (enlarged) {
            ImGui::SameLine();
            if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleVar(2);
    };
    toolbar(false);
    if (!state.running && state.pending_saves == 0 && state.outputs.size() > 1) {
        int choice = gallery_index_ < 0 ? int(state.outputs.size()) - 1 : gallery_index_;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderInt("##result_index", &choice, 0, int(state.outputs.size()) - 1, "Result %d")) {
            std::string error; Raster image;
            if (load_image_rgba(std::filesystem::u8path(state.outputs[choice]), image.pixels, image.w, image.h, error)) {
                gallery_index_ = choice; gallery_image_ = std::move(image);
                pixels = &gallery_image_.pixels; image_width = gallery_image_.w; image_height = gallery_image_.h;
                try { upload_image_texture(preview_texture_, pixels->data(), image_width, image_height); }
                catch(const std::exception& e) { save_message_=e.what(); }
            } else save_message_ = error;
        }
    }
    const float footer = ImGui::GetFrameHeightWithSpacing();
    draw_output_canvas(*pixels, image_width, image_height, ImVec2(0, std::max(1.f, ImGui::GetContentRegionAvail().y - footer)));
    if (state.running) ImGui::ProgressBar(state.progress, ImVec2(-1, 0), state.status.c_str());
    else {
        if (!state.save_error.empty()) {
            ImGui::TextColored(ImVec4(1.f, .45f, .35f, 1.f), "Background save failed (hover for details)");
            ImGui::SetItemTooltip("%s", state.save_error.c_str());
        } else if (state.pending_saves > 0)
            ImGui::TextDisabled("Result ready | Saving %d image(s) in background...", state.pending_saves);
        else ImGui::TextDisabled("%s", state.status.c_str());
        if (!save_message_.empty()) ImGui::SetItemTooltip("%s", save_message_.c_str());
    }
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * .92f, viewport->WorkSize.y * .92f), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * .5f,
                                  viewport->WorkPos.y + viewport->WorkSize.y * .5f), ImGuiCond_Appearing, ImVec2(.5f, .5f));
    if (ImGui::BeginPopupModal("Inspect result", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        toolbar(true);
        draw_output_canvas(*pixels, image_width, image_height, ImVec2(0, std::max(1.f, ImGui::GetContentRegionAvail().y)));
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::End();
}


void App::draw_status_bar() {
    if(!show_telemetry_)return;
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 25.0f);
    ImGui::Separator();
    const Telemetry telemetry = read_telemetry();
    if (telemetry.gpu >= 0.0f)
        ImGui::TextDisabled("FPS %.0f   CPU %.0f%%   GPU %.0f%%   RAM %.1f / %.1f GB   VRAM %.1f / %.1f GB",
                            ImGui::GetIO().Framerate, telemetry.cpu, telemetry.gpu, telemetry.ram_used_gb, telemetry.ram_total_gb,
                            telemetry.vram_used_gb, telemetry.vram_total_gb);
    else
        ImGui::TextDisabled("FPS %.0f   CPU %.0f%%   RAM %.1f / %.1f GB   GPU/VRAM unavailable",
                            ImGui::GetIO().Framerate, telemetry.cpu, telemetry.ram_used_gb, telemetry.ram_total_gb);
}

void App::update_preview_texture() {
    const auto& state = frame_snapshot_;
    if (state.revision == uploaded_revision_ || state.rgba.empty()) return;
    try {
        upload_image_texture(preview_texture_, state.rgba.data(), state.width, state.height);
    } catch (const std::exception& e) {
        save_message_ = e.what();
    }
    uploaded_revision_ = state.revision;
}

void App::refresh_models() {
    const auto previous=selected_model_>=0&&selected_model_<int(models_.size())?models_[selected_model_]:std::filesystem::path{};
    models_.clear();
    model_labels_.clear();
    model_sizes_.clear();
    model_paths_.clear();
    selected_model_ = -1;
    std::error_code error;
    const std::filesystem::path root = std::filesystem::u8path(model_directory_);
    if (!std::filesystem::is_directory(root, error)) {filter_models();return;}
    auto iterator = std::filesystem::recursive_directory_iterator(
        root, std::filesystem::directory_options::skip_permission_denied, error);
    const auto end = std::filesystem::recursive_directory_iterator();
    for (; iterator != end; iterator.increment(error)) {
        if (error) { error.clear(); continue; }
        const auto& entry = *iterator;
        if (entry.is_directory(error) && std::filesystem::is_regular_file(entry.path() / "model_index.json", error)) {
            models_.push_back(entry.path());
            iterator.disable_recursion_pending();
            continue;
        }
        if (!entry.is_regular_file(error)) continue;
        std::string extension = entry.path().extension().string();
        std::ranges::transform(extension, extension.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (extension == ".safetensors" || extension == ".ckpt" || extension == ".gguf") models_.push_back(entry.path());
    }
    std::ranges::sort(models_);
    // Filesystem work belongs in Refresh, not in the render loop.
    for (const auto& path : models_) {
        std::error_code size_error;
        std::uintmax_t bytes = 0;
        bool known = true;
        if (std::filesystem::is_directory(path, size_error)) {
            auto files = std::filesystem::recursive_directory_iterator(path,
                std::filesystem::directory_options::skip_permission_denied, size_error);
            const auto last = std::filesystem::recursive_directory_iterator();
            if (size_error) known = false;
            for (; files != last; files.increment(size_error)) {
                if (size_error) { known = false; size_error.clear(); continue; }
                if (files->is_regular_file(size_error)) {
                    const auto count = files->file_size(size_error);
                    if (size_error) known = false; else bytes += count;
                } else if (size_error) known = false;
            }
            if (size_error) known = false;
        } else {
            bytes = std::filesystem::file_size(path, size_error);
            known = !size_error;
        }
        char size_label[64];
        if (!known) std::snprintf(size_label, sizeof(size_label), "size unavailable");
        else if (bytes >= 1024ULL * 1024 * 1024)
            std::snprintf(size_label, sizeof(size_label), "%.2f GiB", double(bytes) / (1024.0 * 1024 * 1024));
        else std::snprintf(size_label, sizeof(size_label), "%.1f MiB", double(bytes) / (1024.0 * 1024));
        model_labels_.push_back(path_to_utf8(path.filename()));
        model_sizes_.emplace_back(size_label);
        model_paths_.push_back(path_to_utf8(path));
    }
    if (!models_.empty()) {const auto it=std::ranges::find(models_,previous);selected_model_=it==models_.end()?0:int(it-models_.begin());}
    filter_models();
}

void App::refresh_embeddings() {
    embedding_names_.clear();
    std::error_code error;
    const auto root = std::filesystem::u8path(embedding_directory_);
    if (!std::filesystem::is_directory(root, error)) return;
    for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
        if (error) { error.clear(); continue; }
        if (!entry.is_regular_file(error)) continue;
        std::string extension = entry.path().extension().string();
        std::ranges::transform(extension, extension.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (extension == ".pt" || extension == ".bin" || extension == ".safetensors")
            embedding_names_.push_back(path_to_utf8(entry.path().stem()));
    }
    std::ranges::sort(embedding_names_);
}

void App::choose_model_folder() {
#ifdef _WIN32
    IFileDialog* dialog = nullptr;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                                   IID_IFileDialog, reinterpret_cast<void**>(&dialog)))) {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        if (SUCCEEDED(dialog->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR wide_path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &wide_path))) {
                    model_directory_ = path_to_utf8(std::filesystem::path(wide_path));
                    CoTaskMemFree(wide_path);
                    refresh_models();
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    CoUninitialize();
#endif
}

void App::choose_embedding_folder() {
#ifdef _WIN32
    IFileDialog* dialog = nullptr;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                                   IID_IFileDialog, reinterpret_cast<void**>(&dialog)))) {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        if (SUCCEEDED(dialog->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR wide_path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &wide_path))) {
                    embedding_directory_ = path_to_utf8(std::filesystem::path(wide_path));
                    CoTaskMemFree(wide_path);
                    refresh_embeddings();
                    save_settings();
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    CoUninitialize();
#endif
}

void App::choose_lora_folder() {
#ifdef _WIN32
    IFileDialog* dialog = nullptr;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                                   IID_IFileDialog, reinterpret_cast<void**>(&dialog)))) {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        if (SUCCEEDED(dialog->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR wide_path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &wide_path))) {
                    lora_directory_ = path_to_utf8(std::filesystem::path(wide_path));
                    CoTaskMemFree(wide_path);
                    save_settings();
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    CoUninitialize();
#endif
}

void App::save_current_image() {
#ifdef _WIN32
    auto state = backend_.snapshot();
    if(!state.running&&gallery_index_>=0&&!gallery_image_.empty()){state.rgba=gallery_image_.pixels;state.width=gallery_image_.w;state.height=gallery_image_.h;}
    if (state.rgba.empty()) return;

    IFileSaveDialog* dialog = nullptr;
    const HRESULT init_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(init_result) && init_result != RPC_E_CHANGED_MODE) {
        save_message_ = "Could not initialize the save dialog.";
        return;
    }
    if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_ALL,
                                   IID_IFileSaveDialog, reinterpret_cast<void**>(&dialog)))) {
        const COMDLG_FILTERSPEC filters[] = {
            {L"PNG image (*.png)", L"*.png"},
            {L"JPEG image (*.jpg;*.jpeg)", L"*.jpg;*.jpeg"},
        };
        dialog->SetFileTypes(2, filters);
        dialog->SetFileTypeIndex(1);
        dialog->SetDefaultExtension(L"png");
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_s(&local, &now);
        wchar_t suggested_name[64]{};
        std::wcsftime(suggested_name, std::size(suggested_name), L"fox_%Y-%m-%d_%H-%M-%S.png", &local);
        dialog->SetFileName(suggested_name);
        if (SUCCEEDED(dialog->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR wide_path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &wide_path))) {
                    std::filesystem::path path(wide_path);
                    CoTaskMemFree(wide_path);
                    if (path.extension().empty()) {
                        UINT type_index = 1;
                        dialog->GetFileTypeIndex(&type_index);
                        path.replace_extension(type_index == 2 ? L".jpg" : L".png");
                    }
                    std::string error;
                    if (save_image(path, state.rgba, state.width, state.height, 92, error)) {
                        save_message_ = "Saved: " + path.filename().string();
                    } else {
                        save_message_ = std::move(error);
                    }
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    if (SUCCEEDED(init_result)) CoUninitialize();
#endif
}

void App::start_generation() {
    gallery_index_=-1;gallery_image_={};
    if (random_seed_) {
        std::random_device entropy;
        seed_ = (static_cast<std::uint32_t>(entropy()) << 16) ^ static_cast<std::uint32_t>(entropy());
    }
    save_settings();
    GenerationRequest request;
    backend_.set_auto_save(auto_save_txt_,auto_save_img_);
    request.vae_path=qwen_vae_;request.llm_path=qwen_llm_;request.vision_path=qwen_vision_;request.offload_to_cpu=qwen_offload_;
    request.positive_prompt = positive_prompt_;
    request.negative_prompt = negative_prompt_;
    request.steps = steps_;
    request.width = width_;
    request.height = height_;
    request.seed = seed_;
    request.random_seed = random_seed_;
    request.sampling_method = sampler_values[static_cast<std::size_t>(sampler_)];
    request.scheduler = scheduler_values[static_cast<std::size_t>(scheduler_)];
    request.cfg_scale = cfg_scale_;
    request.preview_mode = preview_mode_;
    request.preview_interval = preview_interval_;
    request.preview_denoised = preview_denoised_;
    request.preview_noisy = preview_noisy_;
    request.lora_directory = lora_directory_;
    request.embedding_directory = embedding_directory_;
    if(img2img_active_)request.img2img=img2img_.capture();
    if (selected_model_ >= 0 && selected_model_ < static_cast<int>(models_.size())) {
        request.model_path = path_to_utf8(models_[selected_model_]);
    }
    request.qwen_image=qwen_selected();
    if(request.qwen_image) { request.width=request.width/32*32;request.height=request.height/32*32; }
    backend_.start(std::move(request));
}

void App::load_settings() {
    std::ifstream input(settings_directory() / L"settings.ini", std::ios::binary);
    if (!input) input.open(application_directory() / L"settings.ini", std::ios::binary);
    if (!input) return;

    std::string key;
    std::string selected_model_path;
    while (input >> key) {
        if (key == "version") { int ignored = 0; input >> ignored; }
        else if (key == "positive_prompt") input >> std::quoted(positive_prompt_);
        else if (key == "negative_prompt") input >> std::quoted(negative_prompt_);
        else if (key == "model_directory") input >> std::quoted(model_directory_);
        else if (key == "embedding_directory") input >> std::quoted(embedding_directory_);
        else if (key == "lora_directory") input >> std::quoted(lora_directory_);
        else if (key == "model_hash") input >> std::quoted(model_hash_);
        else if (key == "selected_model") input >> std::quoted(selected_model_path);
        else if(key=="ui_color") { std::string name;std::array<float,4> color;input>>std::quoted(name)>>color[0]>>color[1]>>color[2]>>color[3];
            if(input && std::ranges::all_of(color,[](float v){return std::isfinite(v);})) {for(auto& v:color)v=std::clamp(v,0.f,1.f);ui_colors_[name]=color;} }
        else if (key == "auto_save_txt") input >> auto_save_txt_;
        else if (key == "auto_save_img") input >> auto_save_img_;
        else if (key == "model_type") input >> model_type_;
        else if (key == "qwen_vae") input >> std::quoted(qwen_vae_);
        else if (key == "qwen_llm") input >> std::quoted(qwen_llm_);
        else if (key == "qwen_vision") input >> std::quoted(qwen_vision_);
        else if (key == "qwen_offload") input >> qwen_offload_;
        else if (key == "settings_blur") input >> settings_blur_enabled_;
        else if (key == "show_telemetry") input >> show_telemetry_;
        else if (key == "idle_fps") input >> idle_fps_;
        else if (key == "steps") input >> steps_;
        else if (key == "width") input >> width_;
        else if (key == "height") input >> height_;
        else if (key == "seed") input >> seed_;
        else if (key == "random_seed") input >> random_seed_;
        else if (key == "sampler") input >> sampler_;
        else if (key == "scheduler") input >> scheduler_;
        else if (key == "cfg_scale") input >> cfg_scale_;
        else if (key == "preview_mode") input >> preview_mode_;
        else if (key == "preview_interval") input >> preview_interval_;
        else if (key == "preview_denoised") input >> preview_denoised_;
        else if (key == "preview_noisy") input >> preview_noisy_;
        else if (key == "preview_area_height") input >> preview_area_height_;
        else if (key == "prompt_one_per_line") input >> prompt_one_per_line_;
        else if (key == "show_demo") input >> show_demo_window_;
        else if (key == "workspace_img2img") input >> img2img_active_;
        else if (key == "img2img_settings"||key=="img2img_workflow"||key=="img2img_png_info") img2img_.load_setting(key,input);
        else { std::string ignored; std::getline(input, ignored); }
        if (!input) break;
    }

    idle_fps_=std::clamp(idle_fps_,5,60);model_type_=std::clamp(model_type_,0,2);
    backend_.set_auto_save(auto_save_txt_,auto_save_img_);
    steps_ = std::clamp(steps_, 1, 100);
    width_ = std::clamp(width_, 64, 2048);
    height_ = std::clamp(height_, 64, 2048);
    sampler_ = std::clamp(sampler_, 0, static_cast<int>(sampler_values.size()) - 1);
    scheduler_ = std::clamp(scheduler_, 0, static_cast<int>(scheduler_values.size()) - 1);
    cfg_scale_ = std::clamp(cfg_scale_, 1.0f, 20.0f);
    preview_mode_ = std::clamp(preview_mode_, 0, 3);
    preview_interval_ = std::clamp(preview_interval_, 1, 20);

    if (!model_directory_.empty()) {
        refresh_models();
        const auto wanted = std::filesystem::u8path(selected_model_path);
        const auto found = std::ranges::find(models_, wanted);
        if (found != models_.end()) selected_model_ = static_cast<int>(std::distance(models_.begin(), found));
    }
    if (!embedding_directory_.empty()) refresh_embeddings();
}

void App::save_settings() const {
    const auto directory = settings_directory();
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    std::ofstream output(directory / L"settings.ini", std::ios::binary | std::ios::trunc);
    if (!output) return;

    std::string selected_model_path;
    if (selected_model_ >= 0 && selected_model_ < static_cast<int>(models_.size()))
        selected_model_path = path_to_utf8(models_[selected_model_]);

    output << "version 1\n"
           << "positive_prompt " << std::quoted(positive_prompt_) << '\n'
           << "negative_prompt " << std::quoted(negative_prompt_) << '\n'
           << "model_directory " << std::quoted(model_directory_) << '\n'
           << "embedding_directory " << std::quoted(embedding_directory_) << '\n'
           << "lora_directory " << std::quoted(lora_directory_) << '\n'
           << "model_hash " << std::quoted(model_hash_) << '\n'
           << "selected_model " << std::quoted(selected_model_path) << '\n'
           << "steps " << steps_ << '\n'
           << "width " << width_ << '\n'
           << "height " << height_ << '\n'
           << "seed " << seed_ << '\n'
           << "random_seed " << random_seed_ << '\n'
           << "sampler " << sampler_ << '\n'
           << "scheduler " << scheduler_ << '\n'
           << "cfg_scale " << std::setprecision(9) << cfg_scale_ << '\n'
           << "preview_mode " << preview_mode_ << '\n'
           << "preview_interval " << preview_interval_ << '\n'
           << "preview_denoised " << preview_denoised_ << '\n'
           << "preview_noisy " << preview_noisy_ << '\n'
           << "preview_area_height " << preview_area_height_ << '\n'
           << "prompt_one_per_line " << prompt_one_per_line_ << '\n'
           << "show_demo " << show_demo_window_ << '\n';
    output<<"auto_save_txt "<<auto_save_txt_<<"\nauto_save_img "<<auto_save_img_<<"\nmodel_type "<<model_type_
        <<"\nqwen_vae "<<std::quoted(qwen_vae_)<<"\nqwen_llm "<<std::quoted(qwen_llm_)<<"\nqwen_vision "<<std::quoted(qwen_vision_)
        <<"\nqwen_offload "<<qwen_offload_<<"\nsettings_blur "<<settings_blur_enabled_<<"\nshow_telemetry "<<show_telemetry_<<"\nidle_fps "<<idle_fps_<<"\n";
    for(const auto& [name,c]:ui_colors_)output<<"ui_color "<<std::quoted(name)<<' '<<c[0]<<' '<<c[1]<<' '<<c[2]<<' '<<c[3]<<'\n';
    img2img_.save_settings(output);
    output<<"workspace_img2img "<<img2img_active_<<'\n';
}

void App::apply_theme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = 0.0f;
    style.PopupRounding = 0.0f;
    style.ScrollbarRounding = 0.0f;
    style.GrabRounding = 0.0f;
    style.TabRounding = 0.0f;
    style.ImageRounding = 0.0f;
    style.TreeLinesRounding = 0.0f;
    style.MenuItemRounding = 0.0f;
    style.SelectableRounding = 0.0f;
    style.DragDropTargetRounding = 0.0f;
    style.TabBarBorderSize = 0;
    style.WindowPadding = ImVec2(12, 12);
    style.FramePadding = ImVec2(9, 6);
    style.ItemSpacing = ImVec2(8, 7);
    style.WindowMenuButtonPosition = ImGuiDir_None;

    auto& c = style.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.061f, 0.078f, 1.0f);
    c[ImGuiCol_ChildBg] = ImVec4(0.065f, 0.071f, 0.092f, 1.0f);
    c[ImGuiCol_PopupBg] = ImVec4(0.075f, 0.081f, 0.105f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.17f, 0.19f, 0.24f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.10f, 0.11f, 0.145f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.15f, 0.17f, 0.22f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.18f, 0.20f, 0.27f, 1.0f);
    c[ImGuiCol_TitleBg] = ImVec4(0.045f, 0.050f, 0.065f, 1.0f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.075f, 0.08f, 0.11f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.36f, 0.24f, 0.69f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.46f, 0.32f, 0.83f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.31f, 0.20f, 0.61f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.28f, 0.20f, 0.52f, 0.8f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.39f, 0.28f, 0.70f, 0.9f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.45f, 0.32f, 0.78f, 1.0f);
    c[ImGuiCol_CheckMark] = ImVec4(0.64f, 0.48f, 0.96f, 1.0f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.59f, 0.43f, 0.91f, 1.0f);
    c[ImGuiCol_TabSelected] = ImVec4(0.28f, 0.20f, 0.52f, 1.0f);
    c[ImGuiCol_DockingPreview] = ImVec4(0.55f, 0.38f, 0.93f, 0.65f);
    for(int i=0;i<ImGuiCol_COUNT;++i) {auto found=ui_colors_.find(ImGui::GetStyleColorName(i));
        if(found!=ui_colors_.end()){const auto& v=found->second;c[i]=ImVec4(v[0],v[1],v[2],v[3]);} }
}
#include "app_settings.inl"

#include "app_panels.inl"
