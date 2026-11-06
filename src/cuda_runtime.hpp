#pragma once
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>

std::filesystem::path executable_directory();
std::string cuda_driver_status();
bool cuda_driver_compatible();
// Called only by the inference worker, before any GGML entry point.
void ensure_cuda_runtime(std::stop_token stop, const std::function<void(std::string)> &status);
