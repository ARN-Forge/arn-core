/**
 * @file omniroute_provider.hpp
 * @brief OmniRoute gateway model provider implementation.
 */

#pragma once

#include "arn/core/provider/openai_provider.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace arn::core {

/**
 * @brief Configuration parameters for OmniRoute API requests.
 */
struct OmniRouteConfig {
    /// Base URL endpoint (default "http://localhost:20128").
    std::string endpoint{"http://localhost:20128"};
    /// API path prefix (default "/v1").
    std::string api_path_prefix{"/v1"};
};

/**
 * @brief Model provider implementation for the OmniRoute gateway.
 *
 * Communicates with OmniRoute using its OpenAI-compatible HTTP API.
 * Supports configurable endpoints/base URLs via environment variable
 * (ARN_OMNIROUTE_BASE_URL or OMNIROUTE_BASE_URL) or programmatically.
 */
class OmniRouteProvider final : public OpenAiCompatibleProvider {
public:
    explicit OmniRouteProvider(OmniRouteConfig config = {});
    explicit OmniRouteProvider(std::string endpoint, std::string api_path_prefix = "/v1");
    ~OmniRouteProvider() override;

    OmniRouteProvider(const OmniRouteProvider&) = delete;
    OmniRouteProvider& operator=(const OmniRouteProvider&) = delete;
    OmniRouteProvider(OmniRouteProvider&&) noexcept;
    OmniRouteProvider& operator=(OmniRouteProvider&&) noexcept;

    [[nodiscard]] ProviderType type() const noexcept override {
        return ProviderType::omniroute;
    }

    [[nodiscard]] std::string_view name() const noexcept override {
        return "OmniRoute";
    }

    [[nodiscard]] std::string
    preferred_model(const std::vector<std::string>& models) const override;

    void set_config(OmniRouteConfig config);
    [[nodiscard]] const OmniRouteConfig& config() const noexcept { return config_; }

    void set_base_url(std::string_view url);

private:
    OmniRouteConfig config_;
};

} // namespace arn::core
