#include "arn/core/provider/openrouter_provider.hpp"
#include "provider_utils.hpp"

#include <algorithm>
#include <array>
#include <ranges>
#include <utility>

namespace arn::core {

namespace {

OpenAiProviderConfig make_openai_config(const OpenRouterConfig& cfg) {
    OpenAiProviderConfig result{
        .endpoint = cfg.endpoint,
        .api_path_prefix = cfg.api_path_prefix,
        .provider_name = "OpenRouter",
        .custom_headers = {}
    };
    if (!cfg.http_referer.empty()) {
        result.custom_headers.emplace("HTTP-Referer", cfg.http_referer);
    }
    if (!cfg.app_title.empty()) {
        result.custom_headers.emplace("X-Title", cfg.app_title);
    }
    return result;
}

} // namespace

OpenRouterProvider::OpenRouterProvider(OpenRouterConfig config)
    : OpenAiCompatibleProvider(make_openai_config(config)),
      config_(std::move(config)) {}

OpenRouterProvider::OpenRouterProvider(std::string endpoint, std::string api_path_prefix)
    : OpenRouterProvider(OpenRouterConfig{.endpoint = std::move(endpoint), .api_path_prefix = std::move(api_path_prefix)}) {}

OpenRouterProvider::~OpenRouterProvider() = default;

OpenRouterProvider::OpenRouterProvider(OpenRouterProvider&&) noexcept = default;
OpenRouterProvider& OpenRouterProvider::operator=(OpenRouterProvider&&) noexcept = default;

void OpenRouterProvider::set_config(OpenRouterConfig config) {
    config_ = std::move(config);
    set_provider_config(make_openai_config(config_));
}

std::string OpenRouterProvider::preferred_model(const std::vector<std::string>& models) const {
    static constexpr std::array preferred_candidates = {
        "anthropic/claude-3.7-sonnet",
        "anthropic/claude-3.5-sonnet",
        "openai/gpt-4o",
        "deepseek/deepseek-chat",
        "openai/gpt-4o-mini",
        "google/gemini-2.0-flash-001",
        "meta-llama/llama-3.3-70b-instruct",
        "meta-llama/llama-3.1-70b-instruct"
    };
    for (const auto candidate : preferred_candidates) {
        if (std::ranges::find(models, candidate) != models.end())
            return candidate;
    }
    for (const auto candidate : {"claude-3.5-sonnet", "gpt-4o", "deepseek-chat"}) {
        for (const auto& model : models) {
            if (model.find(candidate) != std::string::npos)
                return model;
        }
    }
    return models.empty() ? std::string{} : models.front();
}

} // namespace arn::core
