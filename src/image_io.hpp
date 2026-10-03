#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

std::vector<std::uint8_t> encode_png(const std::vector<std::uint8_t>& rgba, int width, int height);
bool save_png_parameters(const std::filesystem::path& path, const std::vector<std::uint8_t>& rgba, int width, int height, const std::string& parameters, std::string& error);
std::string read_png_parameters(const std::filesystem::path& path);

bool save_image(const std::filesystem::path& path,
                const std::vector<std::uint8_t>& rgba,
                int width,
                int height,
                int jpeg_quality,
                std::string& error);

bool load_image_rgba(const std::filesystem::path& path,
                     std::vector<std::uint8_t>& rgba,
                     int& width,
                     int& height,
                     std::string& error);

bool load_image_rgba_memory(const std::vector<std::uint8_t>& encoded,
                            std::vector<std::uint8_t>& rgba,
                            int& width,
                            int& height,
                            std::string& error);
