/**
 * @file openai_provider.hpp
 * @brief Reusable OpenAI-compatible model provider base implementation.
 */

#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include "arn/core/provider/model_provider.hpp"

namespace httplib {
class Client;
}

namespace arn::core {

/**
 * @brief Configuration parameters for OpenAI-compatible API providers.
 */
struct OpenAiProviderConfig {
    /// Base URL endpoint (scheme + host + optional port), e.g. "http://127.0.0.1:8000" or "https://api.omniroute.ai".
    std::string endpoint{"http://127.0.0.1:8000"};
    /// API path prefix (e.g. "/v1" or "/api/v1").
    std::string api_path_prefix{"/v1"};
    /// Human-readable provider name for logs, status and errors.
    std::string provider_name{"OpenAI"};
    /// Extra headers sent on every request (e.g. HTTP-Referer, X-Title).
    std::map<std::string, std::string> custom_headers;
};

/**
 * @brief Splits a user-supplied base URL string into an endpoint and API path prefix.
 * @param url Full or partial base URL (e.g. "http://localhost:8000/v1", "https://api.omniroute.ai").
 * @param default_prefix Default path prefix when URL does not specify a path (default "/v1").
 * @return Pair of {endpoint, path_prefix}.
 * @throws std::invalid_argument for unsafe or malformed URLs (without echoing the URL).
 */
[[nodiscard]] std::pair<std::string, std::string>
parse_base_url(std::string_view url, std::string_view default_prefix = "/v1");

/**
 * @brief Base implementation for providers exposing an OpenAI-compatible HTTP API.
 *
 * Implements standard model discovery (GET /models), chat completions (POST /chat/completions),
 * SSE token streaming, function/tool calling schema, and multi-turn conversation tracking.
 */
class OpenAiCompatibleProvider : public IModelProvider {
public:
    explicit OpenAiCompatibleProvider(OpenAiProviderConfig config);
    ~OpenAiCompatibleProvider() override;

    OpenAiCompatibleProvider(const OpenAiCompatibleProvider&) = delete;
    OpenAiCompatibleProvider& operator=(const OpenAiCompatibleProvider&) = delete;
    OpenAiCompatibleProvider(OpenAiCompatibleProvider&&) noexcept;
    OpenAiCompatibleProvider& operator=(OpenAiCompatibleProvider&&) noexcept;

    [[nodiscard]] ApiResult
    list_models(const std::string& api_key,
                const std::atomic_bool* cancel_requested = nullptr) override;

    [[nodiscard]] ModelTurn
    start_turn(const std::string& api_key, const std::string& model,
               const std::string& system_instruction, const std::string& user_prompt,
               const ToolRegistry& tools,
               const StreamCallbacks& callbacks = {},
               const std::atomic_bool* cancel_requested = nullptr) override;

    [[nodiscard]] ModelTurn
    continue_turn(const std::string& api_key, const std::string& model,
                  const std::string& system_instruction,
                  const std::vector<ToolResponse>& tool_responses,
                  const ToolRegistry& tools,
                  const StreamCallbacks& callbacks = {},
                  const std::atomic_bool* cancel_requested = nullptr) override;

    void trim_history(std::size_t max_entries) override;
    void cancel_active_request() override;
    void reset_session() override;
    [[nodiscard]] std::size_t session_entries() const noexcept override;

    [[nodiscard]] ModelCapabilities model_capabilities(std::string_view model) const override;

    /**
     * @brief Updates the provider's base URL and reinitializes the HTTP client.
     * @param url Base URL string (e.g. "http://localhost:8000/v1" or "https://api.omniroute.ai").
     */
    void set_base_url(std::string_view url);

    /**
     * @brief Returns the full configured base URL (endpoint + api_path_prefix).
     */
    [[nodiscard]] std::string base_url() const;

    void set_provider_config(OpenAiProviderConfig config);
    [[nodiscard]] const OpenAiProviderConfig& provider_config() const noexcept { return config_; }

protected:
    void init_client();

    [[nodiscard]] ModelTurn
    execute_turn_request(const std::string& api_key, const std::string& model,
                         const ToolRegistry& tools,
                         const StreamCallbacks& callbacks,
                         const std::atomic_bool* cancel_requested);

    std::string configuration_error_;
    OpenAiProviderConfig config_;
    std::unique_ptr<httplib::Client> client_;
    nlohmann::json messages_ = nlohmann::json::array();
    std::map<std::string, ModelCapabilities, std::less<>> model_capabilities_;
};

} // namespace arn::core
