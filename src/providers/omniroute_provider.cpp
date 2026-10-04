#include "arn/core/provider/omniroute_provider.hpp"
#include "provider_utils.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace arn::core {

namespace {

OpenAiProviderConfig make_openai_config(const OmniRouteConfig& cfg) {
    return OpenAiProviderConfig{
        .endpoint = cfg.endpoint,
        .api_path_prefix = cfg.api_path_prefix,
        .provider_name = "OmniRoute",
        .custom_headers = {}
    };
}

OmniRouteConfig resolve_default_config(OmniRouteConfig initial) {
    const char* override = std::getenv("ARN_OMNIROUTE_BASE_URL");
    if (!override || !*override) override = std::getenv("OMNIROUTE_BASE_URL");
    if (!override || !*override) return initial;
    try {
        auto [endpoint, prefix] = parse_base_url(override, initial.api_path_prefix);
        initial.endpoint = std::move(endpoint);
        initial.api_path_prefix = std::move(prefix);
    } catch (const std::invalid_argument&) {
        // Keep invalid overrides invalid; never send a key to a fallback address.
        // The base provider reports a safe configuration error before any request.
        initial.endpoint = override;
    }
    return initial;
}

} // namespace

OmniRouteProvider::OmniRouteProvider(OmniRouteConfig config)
    : OpenAiCompatibleProvider(make_openai_config(resolve_default_config(std::move(config)))) {
    config_.endpoint = provider_config().endpoint;
    config_.api_path_prefix = provider_config().api_path_prefix;
}

OmniRouteProvider::OmniRouteProvider(std::string endpoint, std::string api_path_prefix)
    : OmniRouteProvider(OmniRouteConfig{.endpoint = std::move(endpoint), .api_path_prefix = std::move(api_path_prefix)}) {}

OmniRouteProvider::~OmniRouteProvider() = default;

OmniRouteProvider::OmniRouteProvider(OmniRouteProvider&&) noexcept = default;
OmniRouteProvider& OmniRouteProvider::operator=(OmniRouteProvider&&) noexcept = default;

void OmniRouteProvider::set_config(OmniRouteConfig config) {
    config_ = std::move(config);
    set_provider_config(make_openai_config(config_));
}

void OmniRouteProvider::set_base_url(std::string_view url) {
    OpenAiCompatibleProvider::set_base_url(url);
    config_.endpoint = OpenAiCompatibleProvider::config_.endpoint;
    config_.api_path_prefix = OpenAiCompatibleProvider::config_.api_path_prefix;
}

std::string OmniRouteProvider::preferred_model(const std::vector<std::string>& models) const {
    if (models.empty()) return {};

    // Standard preferred candidates across common multi-model routes
    static constexpr std::array preferred_candidates = {
        "kiro",
        "anthropic/claude-3.7-sonnet",
        "anthropic/claude-3.5-sonnet",
        "openai/gpt-4o",
        "deepseek/deepseek-chat",
        "google/gemini-2.0-flash",
        "meta-llama/llama-3.3-70b-instruct"
    };

    for (const auto candidate : preferred_candidates) {
        if (std::ranges::find(models, candidate) != models.end())
            return candidate;
    }

    for (const auto candidate : {"kiro", "claude-3.5-sonnet", "gpt-4o", "deepseek-chat"}) {
        for (const auto& model : models) {
            if (model.find(candidate) != std::string::npos)
                return model;
        }
    }

    return models.front();
}

} // namespace arn::core
