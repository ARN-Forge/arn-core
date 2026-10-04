/**
 * @file openrouter_provider.hpp
 * @brief OpenRouter multi-model gateway provider implementation.
 */

#pragma once

#include "arn/core/provider/openai_provider.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace arn::core {

/**
 * @brief Configuration parameters for OpenRouter API requests.
 */
struct OpenRouterConfig {
    /// Base URL endpoint (default "https://openrouter.ai").
    std::string endpoint{"https://openrouter.ai"};
    /// API path prefix (default "/api/v1").
    std::string api_path_prefix{"/api/v1"};
    /// Optional HTTP-Referer header for OpenRouter analytics and rankings.
    std::string http_referer;
    /// Optional X-Title header identifying the application to OpenRouter.
    std::string app_title;
};

/**
 * @brief Model provider implementation for the OpenRouter multi-model gateway.
 *
 * Provides access to hundreds of LLM models through a unified OpenAI-compatible endpoint.
 * Automatically inspects model catalog metadata to discover per-model capabilities
 * (such as tool-calling support) and adjusts request payloads accordingly.
 */
class OpenRouterProvider final : public OpenAiCompatibleProvider {
public:
    /**
     * @brief Constructs an OpenRouterProvider with custom configuration.
     * @param config OpenRouter configuration settings.
     */
    explicit OpenRouterProvider(OpenRouterConfig config = {});

    /**
     * @brief Constructs an OpenRouterProvider with endpoint and path prefix.
     * @param endpoint Base URL endpoint.
     * @param api_path_prefix API path prefix (default "/api/v1").
     */
    explicit OpenRouterProvider(std::string endpoint, std::string api_path_prefix = "/api/v1");
    ~OpenRouterProvider() override;

    OpenRouterProvider(const OpenRouterProvider&) = delete;
    OpenRouterProvider& operator=(const OpenRouterProvider&) = delete;
    OpenRouterProvider(OpenRouterProvider&&) noexcept;
    OpenRouterProvider& operator=(OpenRouterProvider&&) noexcept;

    [[nodiscard]] ProviderType type() const noexcept override {
        return ProviderType::openrouter;
    }

    [[nodiscard]] std::string_view name() const noexcept override {
        return "OpenRouter";
    }

    [[nodiscard]] std::string
    preferred_model(const std::vector<std::string>& models) const override;

    void set_config(OpenRouterConfig config);
    [[nodiscard]] const OpenRouterConfig& config() const noexcept { return config_; }

private:
    OpenRouterConfig config_;
};

} // namespace arn::core
