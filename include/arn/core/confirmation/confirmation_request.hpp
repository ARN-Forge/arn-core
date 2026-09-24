/**
 * @file confirmation_request.hpp
 * @brief Primitives for human-in-the-loop tool execution confirmations.
 */

#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <nlohmann/json.hpp>

namespace arn::core {

/**
 * @brief Represents a request for human-in-the-loop confirmation before executing a tool.
 *
 * Contains metadata about the tool call, parsed arguments, human-readable summary,
 * mutation flags, and optional preview payload (such as file diffs or command strings).
 */
struct ConfirmationRequest {
    /// Name of the tool requesting approval.
    std::string name;
    /// Invocation arguments parsed as JSON.
    nlohmann::json arguments;
    /// Human-readable description of what the tool will do.
    std::string summary;
    /// Indicates whether the action modifies persistent state or resources.
    bool changes_state{false};
    /// Compatibility alias indicating filesystem modification.
    bool changes_files{false};
    /// Optional structured preview data (e.g. diffs or command lines).
    nlohmann::json preview;

    ConfirmationRequest() = default;

    /**
     * @brief Constructs a new ConfirmationRequest with specified attributes.
     * @param name_in Tool name.
     * @param args_in Invocation arguments.
     * @param summary_in Human-readable action summary.
     * @param changes_state_in Whether the action modifies state.
     * @param changes_files_in Whether the action touches files.
     * @param preview_in Optional preview data.
     */
    ConfirmationRequest(std::string name_in,
                        nlohmann::json args_in,
                        std::string summary_in = {},
                        bool changes_state_in = false,
                        bool changes_files_in = false,
                        nlohmann::json preview_in = {})
        : name(std::move(name_in)),
          arguments(std::move(args_in)),
          summary(std::move(summary_in)),
          changes_state(changes_state_in),
          changes_files(changes_files_in || changes_state_in),
          preview(std::move(preview_in)) {}

    /// Returns the name of the requesting tool.
    [[nodiscard]] const std::string& tool_name() const noexcept { return name; }
};

/**
 * @brief Abstract interface for confirmation handling.
 *
 * Applications implement this interface to prompt the user (CLI prompt, GUI dialog,
 * automated policy engine) before executing potentially dangerous tools.
 */
class IConfirmationHandler {
public:
    virtual ~IConfirmationHandler() = default;

    /**
     * @brief Prompts for confirmation or evaluates approval policy.
     * @param request Details of the tool action requesting confirmation.
     * @return True if approved, false if rejected.
     */
    [[nodiscard]] virtual bool confirm(const ConfirmationRequest& request) = 0;
};

/**
 * @brief Functional callback type for confirming tool execution requests.
 *
 * Receives a ConfirmationRequest and returns true to approve or false to deny.
 */
using ConfirmationFn = std::function<bool(const ConfirmationRequest&)>;

/**
 * @brief Wraps an IConfirmationHandler reference into a ConfirmationFn callable.
 * @param handler Reference to an IConfirmationHandler instance. The handler must outlive the returned function.
 * @return ConfirmationFn lambda forwarding to handler.confirm().
 */
inline ConfirmationFn make_confirmation_fn(IConfirmationHandler& handler) {
    return [&handler](const ConfirmationRequest& request) {
        return handler.confirm(request);
    };
}

} // namespace arn::core
