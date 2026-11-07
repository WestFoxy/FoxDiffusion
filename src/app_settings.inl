// Settings UI shares the app's native Windows/OpenGL context.
void App::choose_component(std::string &path) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IFileOpenDialog *picker = nullptr;
    if (SUCCEEDED(
            CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&picker)))) {
        COMDLG_FILTERSPEC filter{L"Model weights", L"*.gguf;*.safetensors;*.pth"};
        picker->SetFileTypes(1, &filter);
        if (SUCCEEDED(picker->Show(nullptr))) {
            IShellItem *item = nullptr;
            if (SUCCEEDED(picker->GetResult(&item))) {
                PWSTR value = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &value))) {
                    path = path_to_utf8(value);
                    CoTaskMemFree(value);
                }
                item->Release();
            }
        }
        picker->Release();
    }
    if (SUCCEEDED(init))
        CoUninitialize();
}

void App::draw_settings() {
    if (!settings_open_) {
        if (!settings_blur_.tiles.empty())
            clear_image_texture(settings_blur_);
        return;
    }
    const auto *viewport = ImGui::GetMainViewport();
    if (settings_capture_) {
        if (settings_blur_enabled_) {
            try {
                int w = 0, h = 0;
                glfwGetFramebufferSize(window_, &w, &h);
                if (w > 0 && h > 0) {
                    std::vector<uint8_t> screen(checked_image_bytes(w, h));
                    GLint previous = 0;
                    glGetIntegerv(GL_READ_BUFFER, &previous);
                    glReadBuffer(GL_FRONT);
                    glPixelStorei(GL_PACK_ALIGNMENT, 4);
                    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, screen.data());
                    glReadBuffer(previous);
                    Raster a(std::max(1, w / 2), std::max(1, h / 2));
                    for (int y = 0; y < a.h; ++y)
                        for (int x = 0; x < a.w; ++x)
                            std::copy_n(screen.data() + (size_t(h - 1 - y * 2) * w + x * 2) * 4, 4,
                                        a.at(x, y));
                    Raster b = a;
                    constexpr int radius = 2;
                    for (int pass = 0; pass < 2; ++pass)
                        for (int axis = 0; axis < 2; ++axis) {
                            int outer = axis ? a.w : a.h, inner = axis ? a.h : a.w;
                            for (int o = 0; o < outer; ++o) {
                                auto pixel = [&](int i) {
                                    return axis ? a.at(o, std::clamp(i, 0, inner - 1))
                                                : a.at(std::clamp(i, 0, inner - 1), o);
                                };
                                int sum[3]{};
                                for (int i = -radius; i <= radius; ++i)
                                    for (int c = 0; c < 3; ++c)
                                        sum[c] += pixel(i)[c];
                                for (int i = 0; i < inner; ++i) {
                                    auto *dst = axis ? b.at(o, i) : b.at(i, o);
                                    for (int c = 0; c < 3; ++c) {
                                        dst[c] = uint8_t(sum[c] / (2 * radius + 1));
                                        sum[c] += pixel(i + radius + 1)[c] - pixel(i - radius)[c];
                                    }
                                }
                            }
                            std::swap(a, b);
                        }
                    upload_image_texture(settings_blur_, a.pixels.data(), a.w, a.h);
                }
            } catch (const std::exception &e) {
                save_message_ = e.what();
            }
        }
        ImGui::OpenPopup("Settings");
        settings_capture_ = false;
    }
    if (settings_blur_enabled_ && !settings_blur_.tiles.empty()) {
        ImGui::SetNextWindowPos(viewport->Pos);
        ImGui::SetNextWindowSize(viewport->Size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("##settingsBackdrop", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoDocking |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
        draw_image_texture(settings_blur_, ImGui::GetWindowDrawList(), viewport->Pos,
                           viewport->Size.x / settings_blur_.width);
        ImGui::End();
        ImGui::PopStyleVar();
    }
    ImGui::SetNextWindowSize(
        ImVec2(std::min(740.f * ui_scale_percent_ / 100.f, viewport->WorkSize.x - 32), std::min(560.f * ui_scale_percent_ / 100.f, viewport->WorkSize.y - 32)),
        ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * .5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * .5f),
                            ImGuiCond_Always, ImVec2(.5f, .5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    if (ImGui::BeginPopupModal("Settings", &settings_open_,
                               ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoDocking)) {
        bool changed = false;
        ImGui::TextDisabled("FOX DIFFUSION  /  PREFERENCES");
        ImGui::Spacing();
        if (ImGui::BeginTabBar("settingsTabs")) {
            if (ImGui::BeginTabItem("Output")) {
                ImGui::BeginChild("outputPrefs", ImVec2(0, -48));
                ImGui::SeparatorText("Automatic saving");
                changed |= ImGui::Checkbox("Save txt2img results automatically", &auto_save_txt_);
                changed |=
                    ImGui::Checkbox("Save img2img / inpaint / script results automatically", &auto_save_img_);
                ImGui::TextWrapped("When disabled, results stay in memory. Use More > Save as or File > Save "
                                   "image as to keep an image.");
                ImGui::TextDisabled("Already queued saves finish normally. Changes apply to future results.");
                ImGui::Spacing();
                ImGui::SeparatorText("Location");
                ImGui::TextWrapped("Default: output folder beside the executable. Img2img can use its Batch "
                                   "& output > Output directory setting.");
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Interface")) {
                ImGui::BeginChild("interfacePrefs", ImVec2(0, -48));
                ImGui::SeparatorText("Interface scale");
                const int scales[] = {100, 115, 125, 150, 175, 200};
                for (int percent : scales) {
                    if (percent != 100) ImGui::SameLine();
                    char label[16]; std::snprintf(label, sizeof(label), "%d%%", percent);
                    const bool selected = ui_scale_percent_ == percent;
                    if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
                    if (ImGui::Button(label)) { ui_scale_percent_ = percent; changed = true; }
                    if (selected) ImGui::PopStyleColor();
                }
                ImGui::TextWrapped("Text is rasterized at the selected pixel size. Changes apply immediately; 100%% keeps the original pixel font.");
                ImGui::Spacing();
                ImGui::SeparatorText("Workspace");
                changed |= ImGui::Checkbox("Show performance statistics", &show_telemetry_);
                changed |= ImGui::Checkbox("Show terminal / engine logs", &show_console_);
                ImGui::SetItemTooltip("Dock Terminal anywhere. Captures engine logs and stdout/stderr; keeps the latest 4,000 lines.");
                if (ImGui::Checkbox("One prompt per line", &prompt_one_per_line_)) {
                    changed = true;
                    prompt_layout_initialized_ = false;
                }
                changed |= ImGui::Checkbox("Blur background behind settings", &settings_blur_enabled_);
                if (ImGui::Button("Reset current mode layout"))
                    reset_workspace_layout_ = true;
                ImGui::Spacing();
                ImGui::SeparatorText("Redrawing");
                ImGui::SetNextItemWidth(260);
                changed |= ImGui::SliderInt("Idle FPS", &idle_fps_, 5, 60);
                ImGui::TextDisabled("Active interaction uses VSync. Font: standard Dear ImGui.");
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Preview")) {
                ImGui::BeginChild("previewPrefs", ImVec2(0, -48));
                ImGui::SeparatorText("Generation preview");
                ImGui::SetNextItemWidth(260);
                changed |= ImGui::Combo("Mode", &preview_mode_,
                                        "Off\0Projected (fast)\0Tiny Autoencoder\0Full VAE\0");
                ImGui::SetNextItemWidth(260);
                changed |= ImGui::SliderInt("Every N steps", &preview_interval_, 1, 20);
                changed |= ImGui::Checkbox("Denoised preview", &preview_denoised_);
                changed |= ImGui::Checkbox("Noisy preview", &preview_noisy_);
                ImGui::TextWrapped("Preview changes apply at the next generation. ESRGAN shows completed "
                                   "regions while upscaling.");
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Colors")) {
                ImGui::BeginChild("colorPrefs", ImVec2(0, -48));
                ImGui::TextDisabled("Changes apply immediately and are stored in settings.ini.");
                if (ImGui::Button("Reset all colors")) {
                    ui_colors_.clear();
                    applied_ui_scale_ = 0;
                    changed = true;
                }
                static ImGuiTextFilter filter;
                filter.Draw("Find color", 260);
                auto &style = ImGui::GetStyle();
                for (int i = 0; i < ImGuiCol_COUNT; ++i) {
                    const char *name = ImGui::GetStyleColorName(i);
                    if (!filter.PassFilter(name))
                        continue;
                    ImGui::PushID(i);
                    if (ImGui::ColorEdit4(name, &style.Colors[i].x,
                                          ImGuiColorEditFlags_AlphaBar |
                                              ImGuiColorEditFlags_AlphaPreviewHalf)) {
                        const auto &c = style.Colors[i];
                        ui_colors_[name] = {c.x, c.y, c.z, c.w};
                        changed = true;
                    }
                    ImGui::PopID();
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        if (changed) {
            backend_.set_auto_save(auto_save_txt_, auto_save_img_);
            save_settings();
        }
        ImGui::Separator();
        ImGui::TextDisabled("Changes are saved automatically.");
        ImGui::SameLine(ImGui::GetWindowWidth() - 100);
        if (ImGui::Button("Done", ImVec2(72, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            settings_open_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

void App::draw_model_loading() {
    bool changed = false;
    ImGui::TextUnformatted("Model type");
    ImGui::SetNextItemWidth(-1);
    changed |=
        ImGui::Combo("##modelType", &model_type_, "Auto (filename)\0SD / SDXL checkpoint\0Qwen-Image-2.1\0");
    ImGui::TextWrapped("Auto works for named SD/SDXL checkpoints and Qwen-Image-2.1 files.");
    if (ImGui::CollapsingHeader("Qwen-Image-2.1 components",
                                qwen_selected() ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
        ImGui::TextWrapped("Select diffusion weights in Select model. Add the matching components below.");
        auto component = [&](const char *label, std::string &path) {
            ImGui::PushID(label);
            ImGui::TextUnformatted(label);
            ImGui::SetNextItemWidth(-88);
            changed |= ImGui::InputText("##path", &path);
            ImGui::SameLine();
            if (ImGui::Button("Browse")) {
                choose_component(path);
                changed = true;
            }
            ImGui::PopID();
        };
        component("VAE (Qwen 2.1)", qwen_vae_);
        component("Text encoder (Qwen3-VL-8B)", qwen_llm_);
        component("Vision projector (GGUF editing)", qwen_vision_);
        changed |= ImGui::Checkbox("CPU offloading to reduce VRAM", &qwen_offload_);
        if (ImGui::Button("Use recommended Qwen sampling")) {
            sampler_ = 0;
            scheduler_ = 0;
            cfg_scale_ = 6;
            changed = true;
        }
        ImGui::SetItemTooltip("Euler / discrete / CFG 6. Changes apply to the next generation.");
        ImGui::TextWrapped("Editing uses an instruction prompt and a reference image.");
        ImGui::SetItemTooltip("Masks are composited locally; this differs from SD latent inpainting. "
                              "Generation sizes are aligned to 32 pixels.");
    }
    if (changed)
        save_settings();
}
