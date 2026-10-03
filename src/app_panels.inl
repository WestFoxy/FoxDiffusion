std::string App::panel_title(const char *name) const {
    return std::string(name) + "###Fox" + name + (workspace_mode_ ? "Img" : "Txt");
}
bool App::qwen_selected() const {
    if (model_type_ != 0)
        return model_type_ == 2;
    if (selected_model_ < 0 || selected_model_ >= int(model_labels_.size()))
        return false;
    std::string name = model_labels_[selected_model_];
    std::ranges::transform(name, name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return name.find("qwen") != std::string::npos &&
           (name.find("2.1") != std::string::npos || name.find("2_1") != std::string::npos);
}
void App::filter_models() {
    filtered_models_.clear();
    std::string query = model_filter_;
    std::ranges::transform(query, query.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    for (int i = 0; i < int(model_labels_.size()); ++i) {
        std::string name = model_labels_[i];
        std::ranges::transform(name, name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (query.empty() || name.find(query) != std::string::npos)
            filtered_models_.push_back(i);
    }
}
void App::draw_prompt_panel() {
    ImGui::Begin(panel_title("Generation").c_str());
    const auto &state = frame_snapshot_;
    const bool selected = selected_model_ >= 0 && selected_model_ < int(model_labels_.size());
    ImGui::TextDisabled(workspace_mode_ ? "IMAGE EDITING" : "TEXT TO IMAGE");
    if (selected) {
        ImGui::TextUnformatted(model_labels_[selected_model_].c_str());
        ImGui::SetItemTooltip("Selected model: %s", model_paths_[selected_model_].c_str());
    } else
        ImGui::TextDisabled("Select a model in Models to begin.");
    const bool source_ready =
        !workspace_mode_ ||
        (img2img_.options.mode == 5 ? !img2img_.options.input_dir.empty() : img2img_.has_source());
    if (state.running) {
        if (ImGui::Button("Cancel generation", ImVec2(-1, 38)))
            backend_.cancel();
        ImGui::TextWrapped("%s", state.status.c_str());
        ImGui::SetItemTooltip("Edits to parameters apply to your next generation.");
    } else {
        ImGui::BeginDisabled(!selected || !source_ready);
        if (ImGui::Button(workspace_mode_ ? "Generate edit" : "Generate image", ImVec2(-1, 38)))
            start_generation();
        ImGui::EndDisabled();
        if (!source_ready)
            ImGui::TextWrapped(img2img_.options.mode == 5
                                   ? "Choose the batch input directory in the editor."
                                   : "Open or paste an image using Image in the editor.");
    }
    ImGui::BeginChild("generationBody");
    if (!prompt_layout_initialized_) {
        if (prompt_one_per_line_) {
            format_prompt_lines(positive_prompt_);
            format_prompt_lines(negative_prompt_);
        }
        prompt_layout_initialized_ = true;
    }
    ImGui::SeparatorText(workspace_mode_ && qwen_selected() ? "Edit instruction" : "Prompt");
    ImGui::InputTextMultiline("##positive", &positive_prompt_,
                              ImVec2(-1, std::clamp(ImGui::GetWindowHeight() * .24f, 110.f, 180.f)),
                              ImGuiInputTextFlags_NoHorizontalScroll);
    if (prompt_one_per_line_ && ImGui::IsItemDeactivatedAfterEdit())
        format_prompt_lines(positive_prompt_);
    if (ImGui::CollapsingHeader("Avoid / negative prompt")) {
        ImGui::InputTextMultiline("##negative", &negative_prompt_, ImVec2(-1, 90),
                                  ImGuiInputTextFlags_NoHorizontalScroll);
        if (prompt_one_per_line_ && ImGui::IsItemDeactivatedAfterEdit())
            format_prompt_lines(negative_prompt_);
    }
    ImGui::SeparatorText("Generation settings");
    ImGui::TextUnformatted("Steps");
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderInt("##steps", &steps_, 1, 100, "%d steps");
    ImGui::SetItemTooltip("More steps take longer; quality gains eventually level off.");
    ImGui::TextUnformatted("Prompt guidance (CFG)");
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##cfg", &cfg_scale_, 1, 20, "%.1f");
    ImGui::SetItemTooltip(
        "How strongly the model follows the prompt. Higher values can introduce artifacts.");
    if (ImGui::BeginTable("processingSize", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("Width");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputInt("##width", &width_, 64);
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted("Height");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputInt("##height", &height_, 64);
        ImGui::EndTable();
    }
    width_ = std::clamp(width_, 64, 4096);
    height_ = std::clamp(height_, 64, 4096);
    if (workspace_mode_) {
        ImGui::TextDisabled(img2img_.options.script == 1
                                ? "SD upscale: width / height define diffusion tiles."
                                : "Processing size. Resize and mask options are in the editor.");
        ImGui::SetItemTooltip("Only masked area uses this size for its crop; SD upscale uses it for "
                              "diffusion tiles. Scale by in the editor overrides it.");
    }
    if (ImGui::CollapsingHeader("Sampling & seed")) {
        ImGui::TextUnformatted("Sampler");
        ImGui::SetNextItemWidth(-1);
        ImGui::Combo("##sampler", &sampler_, sampler_labels.data(), int(sampler_labels.size()));
        ImGui::TextUnformatted("Scheduler");
        ImGui::SetNextItemWidth(-1);
        ImGui::Combo("##scheduler", &scheduler_, scheduler_labels.data(), int(scheduler_labels.size()));
        ImGui::Checkbox("Random seed for each run", &random_seed_);
        ImGui::BeginDisabled(random_seed_);
        ImGui::TextUnformatted("Seed");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputScalar("##seed", ImGuiDataType_U32, &seed_);
        ImGui::EndDisabled();
    }
    if (ImGui::CollapsingHeader("Prompt tools")) {
        if (ImGui::Checkbox("One tag per line", &prompt_one_per_line_) && prompt_one_per_line_) {
            format_prompt_lines(positive_prompt_);
            format_prompt_lines(negative_prompt_);
        }
        bool active = false;
        for (const auto &name : embedding_names_)
            if (positive_prompt_.find(name) != std::string::npos ||
                negative_prompt_.find(name) != std::string::npos) {
                ImGui::TextWrapped("Embedding in prompt: %s", name.c_str());
                active = true;
            }
        if (!embedding_names_.empty() && !active)
            ImGui::TextDisabled("No embedding filename token in prompt.");
        if (ImGui::CollapsingHeader("Final prompt preview")) {
            ImGui::BeginChild("finalPrompt", ImVec2(0, 100), ImGuiChildFlags_Borders);
            ImGui::TextWrapped("%s", positive_prompt_.c_str());
            ImGui::Separator();
            ImGui::TextWrapped("Negative: %s", negative_prompt_.c_str());
            ImGui::EndChild();
        }
        if (ImGui::Button("Copy prompt"))
            ImGui::SetClipboardText(positive_prompt_.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Copy negative"))
            ImGui::SetClipboardText(negative_prompt_.c_str());
    }
    ImGui::TextDisabled("Output auto-save: %s",
                        (workspace_mode_ ? auto_save_img_ : auto_save_txt_) ? "on" : "off");
    ImGui::SetItemTooltip("Change in File > Settings > Output. Manual Save is in the Result panel.");
    ImGui::EndChild();
    ImGui::End();
}
void App::draw_models_panel() {
    ImGui::Begin(panel_title("Models").c_str());
    if (selected_model_ >= 0 && selected_model_ < int(model_labels_.size())) {
        ImGui::TextDisabled("SELECTED FOR GENERATION");
        ImGui::TextUnformatted(model_labels_[selected_model_].c_str());
        ImGui::SetItemTooltip("%s\n%s", model_paths_[selected_model_].c_str(),
                              model_sizes_[selected_model_].c_str());
    } else
        ImGui::TextDisabled("Choose a model to enable Generate.");
    if (ImGui::BeginTabBar("modelSections")) {
        if (ImGui::BeginTabItem("Select model")) {
            if (ImGui::Button("Folder..."))
                choose_model_folder();
            ImGui::SameLine();
            if (ImGui::Button("Refresh"))
                refresh_models();
            ImGui::SameLine();
            ImGui::TextDisabled("%zu / %zu", filtered_models_.size(), models_.size());
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputTextWithHint("##modelSearch", "Search models...", &model_filter_))
                filter_models();
            if (ImGui::CollapsingHeader("Folder path")) {
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputText("##modeldir", &model_directory_, ImGuiInputTextFlags_EnterReturnsTrue)) {
                    refresh_models();
                    save_settings();
                }
                ImGui::SetItemTooltip("Enter applies the folder. Refresh scans model files and disk sizes.");
            }
            ImGui::BeginChild("modelList", ImVec2(0, 0), ImGuiChildFlags_Borders);
            if (models_.empty())
                ImGui::TextWrapped("Choose a folder with .safetensors, .ckpt or .gguf models.");
            else if (filtered_models_.empty())
                ImGui::TextDisabled("No models match this search.");
            if (ImGui::BeginTable("model_files", 2, ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Model", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 75.f);
                ImGuiListClipper clipper;
                clipper.Begin(int(filtered_models_.size()));
                while (clipper.Step())
                    for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                        const int i = filtered_models_[row];
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::PushID(i);
                        if (ImGui::Selectable(model_labels_[i].c_str(), selected_model_ == i,
                                              ImGuiSelectableFlags_SpanAllColumns)) {
                            selected_model_ = i;
                            save_settings();
                        }
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                            ImGui::SetTooltip("%s\nDisk size: %s", model_paths_[i].c_str(),
                                              model_sizes_[i].c_str());
                        ImGui::PopID();
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextDisabled("%s", model_sizes_[i].c_str());
                    }
                ImGui::EndTable();
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Loading")) {
            ImGui::BeginChild("loadingPrefs");
            draw_model_loading();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Extras")) {
            ImGui::BeginChild("modelExtras");
            if (ImGui::CollapsingHeader("Text embeddings")) {
                ImGui::TextWrapped("Use the embedding filename in your positive or negative prompt.");
                ImGui::SetNextItemWidth(-1);
                ImGui::InputText("##embeddingsdir", &embedding_directory_);
                if (ImGui::Button("Choose folder##embeddings"))
                    choose_embedding_folder();
                ImGui::SameLine();
                if (ImGui::Button("Refresh##embeddings"))
                    refresh_embeddings();
                ImGui::TextDisabled("%zu embedding(s)", embedding_names_.size());
            }
            if (ImGui::CollapsingHeader("LoRA")) {
                ImGui::TextWrapped("LoRA prompt syntax: <lora:filename:weight>");
                ImGui::SetNextItemWidth(-1);
                ImGui::InputText("##loradir", &lora_directory_);
                if (ImGui::Button("Choose folder##loras"))
                    choose_lora_folder();
            }
            if (ImGui::CollapsingHeader("Advanced metadata")) {
                ImGui::TextUnformatted("Model hash override");
                ImGui::SetNextItemWidth(-1);
                ImGui::InputText("##modelhash", &model_hash_);
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}
