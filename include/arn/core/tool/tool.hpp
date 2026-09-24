/**
 * @file tool.hpp
 * @brief Interfaces and types for defining and executing AI agent tools.
 */

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>
#include "arn/core/confirmation/confirmation_request.hpp"

namespace arn::core {

/**
 * @brief Metadata and JSON Schema describing a tool to language models.
 */
struct ToolDefinition {
    /// Unique identifier / invocation name of the tool (e.g. "calculate", "read_file").
    std::string name;
    /// Human and LLM readable summary of the tool's purpose and usage.
    std::string description;
    /// JSON Schema object describing expected invocation parameters and types.
    nlohmann::json parameter_schema;
};

/**
 * @brief Context passed to a tool during invocation.
 *
 * Provides cooperative cancellation checking and an optional confirmation callback
 * for requesting user approval for sensitive actions.
 */
struct ToolContext {
    /// Pointer to atomic cancellation flag, or nullptr if cancellation is not monitored.
    const std::atomic_bool* cancel_requested{nullptr};
    /// Confirmation callback function, or empty function if confirmation is not configured.
    ConfirmationFn confirm;
};

/**
 * @brief Outcome of a tool execution.
 *
 * Contains status flag, structured JSON payload returned to the LLM, and optional error message.
 */
struct ToolResult {
    /// True if the tool completed successfully; false on error.
    bool ok{false};
    /// Structured result payload returned to the model as context.
    nlohmann::json result;
    /// Error description if execution failed.
    std::string error_message;

    ToolResult() = default;

    /**
     * @brief Constructs a ToolResult with explicit fields.
     * @param ok_in Success status.
     * @param result_in JSON result payload.
     * @param err Optional error string.
     */
    ToolResult(bool ok_in, nlohmann::json result_in, std::string err = {})
        : ok(ok_in), result(std::move(result_in)), error_message(std::move(err)) {}

    /**
     * @brief Helper creating a successful ToolResult.
     * @param output JSON object or value containing the tool output.
     * @return Initialized ToolResult with ok = true.
     */
    static ToolResult success(nlohmann::json output) {
        return {true, std::move(output), {}};
    }

    /**
     * @brief Helper creating a failed ToolResult.
     * @param message Description of the error.
     * @return Initialized ToolResult with ok = false and JSON `{"error": message}`.
     */
    static ToolResult failure(std::string message) {
        return {false, {{"error", message}}, message};
    }
};

/**
 * @brief Abstract interface for an agent tool.
 *
 * Concrete tools implement this interface to expose functionality (calculations,
 * web searches, domain logic) to the model via the ToolRegistry.
 */
class ITool {
public:
    virtual ~ITool() = default;

    /**
     * @brief Returns the static definition and JSON Schema for this tool.
     * @return Reference to a ToolDefinition struct.
     */
    [[nodiscard]] virtual const ToolDefinition& definition() const noexcept = 0;

    /**
     * @brief Specifies whether this tool requires human-in-the-loop approval before executing.
     * @return True if confirmation is required; false otherwise (default false).
     */
    [[nodiscard]] virtual bool requires_confirmation() const noexcept { return false; }

    /**
     * @brief Executes the tool with the given arguments and execution context.
     * @param arguments JSON arguments supplied by the model matching parameter_schema.
     * @param context Execution context containing cancellation token and confirmation handler.
     * @return ToolResult indicating success or failure with output data.
     */
    [[nodiscard]] virtual ToolResult execute(const nlohmann::json& arguments,
                                             const ToolContext& context) = 0;
};

} // namespace arn::core
