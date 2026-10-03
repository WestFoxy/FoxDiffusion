#pragma once
#include "generation.hpp"
#include <filesystem>
#include <functional>
#include <stop_token>

using InferImage = std::function<Raster(GenerationRequest)>;
using PublishImage = std::function<void(const Raster &, const GenerationRequest &, int, int)>;
void execute_img2img(GenerationRequest request, std::stop_token stop, const InferImage &infer,
                     const PublishImage &publish);
