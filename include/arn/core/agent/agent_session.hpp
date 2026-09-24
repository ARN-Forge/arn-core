/**
 * @file agent_session.hpp
 * @brief High-level orchestration engine for multi-turn AI agent sessions.
 */

#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "arn/core/confirmation/confirmation_request.hpp"
#include "arn/core/provider/model_provider.hpp"
#include "arn/core/tool/tool_registry.hpp"

namespace arn::core {

/**
 * @brief Configuration parameters for an AgentSession.
 */
struct AgentConfig {
    /// System prompt or instructions guiding model behavior across turns.
    std::string system_instruction;
    /// Maximum consecutive rounds of tool calls allowed within a single user prompt turn (default 12).
    int max_tool_rounds{12};
    /// Maximum number of messages kept in provider conversation history before older entries are pruned (default 40).
    std::size_t max_history_entries{40};
};

/**
 * @brief Central coordinator for multi-turn conversational agents with tool calling.
 *
 * `AgentSession` manages the orchestration loop between a consuming application,
 * an `IModelProvider`, a `ToolRegistry`, and an optional confirmation handler.
 *
 * Lifetimes & Ownership:
 * - Movable, non-copyable.
 * - Thread-safe across method calls (serialized by an internal mutex).
 * - `cancel_active_request()` may be called concurrently from another thread to abort an ongoing prompt.
 */
class AgentSession {
public:
    /**
     * @brief Constructs an AgentSession with given configuration.
     * @param config Initial session configuration settings.
     */
    explicit AgentSession(AgentConfig config = {});
    ~AgentSession();

    AgentSession(const AgentSession&) = delete;
    AgentSession& operator=(const AgentSession&) = delete;
    AgentSession(AgentSession&&) noexcept;
    AgentSession& operator=(AgentSession&&) noexcept;

    /**
     * @brief Sets or updates the active system instruction.
     * @param instruction The system prompt string.
     */
    void set_system_instruction(std::string instruction);

    /// Returns the currently configured system instruction.
    [[nodiscard]] const std::string& system_instruction() const noexcept;

    /**
     * @brief Attaches a shared tool registry for tool discovery and execution.
     * @param tools Shared pointer to the immutable tool registry.
     */
    void set_tools(std::shared_ptr<const ToolRegistry> tools);

    /// Returns a non-owning pointer to the attached tool registry, or nullptr if none set.
    [[nodiscard]] const ToolRegistry* tools() const noexcept;

    /**
     * @brief Sets the callback for requesting human-in-the-loop approval before executing sensitive tools.
     * @param confirm_handler Confirmation callback callable.
     */
    void set_confirmation_handler(ConfirmationFn confirm_handler);

    /// Returns the active confirmation callback function.
    [[nodiscard]] const ConfirmationFn& confirmation_handler() const noexcept;

    /**
     * @brief Configures a provider instance, verifies credentials by listing models, and selects the preferred model.
     * @param provider Shared pointer to the provider instance.
     * @param api_key API key / secret token.
     * @param cancel Optional cancellation flag.
     * @return ApiResult indicating model discovery outcome.
     */
    [[nodiscard]] ApiResult configure_provider(std::shared_ptr<IModelProvider> provider,
                                               std::string api_key,
                                               const std::atomic_bool* cancel = nullptr);

    /**
     * @brief Instantiates and configures a built-in provider by type, discovers models, and selects the preferred model.
     * @param type Built-in provider type (e.g. ProviderType::gemini).
     * @param api_key API key / secret token.
     * @param cancel Optional cancellation flag.
     * @return ApiResult indicating model discovery outcome.
     */
    [[nodiscard]] ApiResult configure_provider(ProviderType type,
                                               const std::string& api_key,
                                               const std::atomic_bool* cancel = nullptr);

    /**
     * @brief Sets an externally managed provider pointer and API key directly without model auto-discovery.
     * @note Caller is responsible for calling @c select_model() after this method.
     * @param provider Non-owning pointer to provider (must outlive session usage).
     * @param api_key API key.
     */
    void set_provider(IModelProvider* provider, std::string api_key);

    /// Returns the list of model IDs discovered during the last provider configuration.
    [[nodiscard]] std::vector<std::string> available_models() const;

    /// Returns the provider's recommended default model ID.
    [[nodiscard]] std::string preferred_model() const;

    /**
     * @brief Sets the active model identifier to use for subsequent prompts.
     * @param model Model identifier (e.g. "gemini-2.0-flash", "deepseek-chat").
     */
    void select_model(std::string model);

    /// Returns the currently active model identifier.
    [[nodiscard]] std::string active_model() const;

    /// Returns the feature capabilities of the currently active model.
    [[nodiscard]] ModelCapabilities model_capabilities() const;

    /**
     * @brief Sends a user prompt to the model and executes the multi-turn tool calling loop.
     * @param text The user prompt message text.
     * @param callbacks Callbacks for streaming tokens and progress updates.
     * @param cancel Optional pointer to atomic cancellation flag.
     * @return ApiResult with the final assistant response text or error details.
     */
    [[nodiscard]] ApiResult prompt(std::string_view text,
                                   const StreamCallbacks& callbacks = {},
                                   const std::atomic_bool* cancel = nullptr);

    /// Overload of prompt() receiving only a text streaming callback.
    [[nodiscard]] ApiResult prompt(std::string_view text,
                                   const TextStreamCallback& on_text) {
        return prompt(text, StreamCallbacks{.on_text = on_text}, nullptr);
    }

    /// Overload of prompt() receiving only a cancellation flag.
    [[nodiscard]] ApiResult prompt(std::string_view text,
                                   const std::atomic_bool* cancel) {
        return prompt(text, StreamCallbacks{}, cancel);
    }

    /// Overload of prompt() receiving text streaming callback, cancellation flag, and progress callback.
    [[nodiscard]] ApiResult prompt(std::string_view text,
                                   const TextStreamCallback& on_text,
                                   const std::atomic_bool* cancel,
                                   const ProgressCallback& on_progress) {
        return prompt(text, StreamCallbacks{.on_text = on_text, .on_progress = on_progress}, cancel);
    }

    /**
     * @brief Aborts the currently executing prompt or HTTP request.
     *
     * Thread-safe. Can be called from any thread (e.g. UI or signal handler).
     */
    void cancel_active_request();

    /// Clears conversation history on the active provider.
    void reset_session();

    /// Returns the number of conversation turns recorded in the active provider session.
    [[nodiscard]] std::size_t session_entries() const noexcept;

    /// Returns the ProviderType of the active provider, or ProviderType::none if none configured.
    [[nodiscard]] ProviderType active_provider() const noexcept;

    /// Returns a non-owning pointer to the underlying IModelProvider instance.
    [[nodiscard]] IModelProvider* provider() const noexcept;

    /// Returns the session configuration parameters.
    [[nodiscard]] const AgentConfig& config() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace arn::core
