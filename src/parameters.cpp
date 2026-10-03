#include "parameters.hpp"
#include <regex>

void apply_parameters(GenerationRequest &r, const std::string &text) {
    auto settings = text.rfind("\nSteps:");
    if (settings == std::string::npos)
        return;
    auto negative = text.find("\nNegative prompt:");
    r.positive_prompt = text.substr(0, negative < settings ? negative : settings);
    if (negative < settings)
        r.negative_prompt = text.substr(negative + 18, settings - negative - 18);
    std::string params = text.substr(settings + 1);
    std::smatch match;
    auto number = [&](const char *key) -> std::string {
        std::regex pattern(std::string("(?:^|, )") + key + ": ([^,\n]+)");
        if (std::regex_search(params, match, pattern))
            return match[1].str();
        return {};
    };
    auto integer = [&](const char *key, int &v, int low, int high) {
        auto s = number(key);
        if (!s.empty()) {
            try {
                v = std::clamp(std::stoi(s), low, high);
            } catch (...) {
            }
        }
    };
    auto real = [&](const char *key, float &v, float low, float high) {
        auto s = number(key);
        if (!s.empty()) {
            try {
                float n = std::stof(s);
                if (std::isfinite(n))
                    v = std::clamp(n, low, high);
            } catch (...) {
            }
        }
    };
    integer("Steps", r.steps, 1, 150);
    real("CFG scale", r.cfg_scale, 1, 30);
    real("Denoising strength", r.img2img.strength, 0, 1);
    auto seed = number("Seed");
    if (!seed.empty()) {
        try {
            auto n = std::stoll(seed);
            if (n >= 0 && n <= 4294967295LL) {
                r.seed = std::uint32_t(n);
                r.random_seed = false;
            }
        } catch (...) {
        }
    }
    auto size = number("Size");
    std::regex dimensions("([0-9]+)x([0-9]+)");
    if (std::regex_match(size, match, dimensions)) {
        try {
            r.width = std::clamp(std::stoi(match[1]), 64, 4096);
            r.height = std::clamp(std::stoi(match[2]), 64, 4096);
        } catch (...) {
        }
    }
    auto sampler = number("Sampler");
    if (!sampler.empty()) {
        const std::pair<const char *, const char *> samplers[] = {
            {"Euler", "euler"},      {"Euler a", "euler_a"},
            {"DPM++ 2M", "dpm++2m"}, {"DPM++ 2M SDE", "dpm++2m_sde"},
            {"Heun", "heun"},        {"DPM2", "dpm2"},
            {"LMS", "lms"}};
        bool mapped = false;
        for (auto p : samplers)
            if (sampler == p.first) {
                r.sampling_method = p.second;
                mapped = true;
                break;
            }
        if (!mapped && sampler.find(' ') == std::string::npos)
            r.sampling_method = sampler;
    }
    auto schedule = number("Schedule type");
    if (!schedule.empty()) {
        for (auto &c : schedule)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        r.scheduler = schedule == "normal" ? "discrete" : schedule;
    }
}
