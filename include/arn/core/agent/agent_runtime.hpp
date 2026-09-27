/** @file agent_runtime.hpp
 * @brief Synchronous execution of one isolated task through AgentSession.
 */
#pragma once
#include "arn/core/agent/agent_context.hpp"
#include "arn/core/agent/agent_result.hpp"
#include "arn/core/agent/agent_session.hpp"
#include <memory>
#include <string>
#include <vector>

namespace arn::core {
/** @brief Trusted host classification of an existing tool; all operations must pass.
 * Empty/unknown classifications are denied. Classify every possible side effect,
 * including command/network effects. Native tools remain trusted code, not sandboxed.
 */
struct AgentToolBinding {
    std::shared_ptr<ITool> tool;
    std::vector<OperationClass> operations;
};

/** @brief Executes one context using an exclusively owned, explicitly injected provider.
 * A fresh AgentSession and filtered registry are created per execute(). Session owns
 * the model/tool loop; runtime owns validation, policy gates and outcome translation.
 * Provider history is reset through AgentSession before and after each execution.
 * Caller contexts are copied, never modified. Tools may be shared but the host must
 * configure their workspace scope/state; working_directory is metadata, not chdir
 * or a filesystem sandbox. Do not access the transferred provider externally.
 *
 * ModelPolicy is preference metadata: no ranking, discovery or capability claims.
 * The explicit model is used as supplied. default_skills are preserved but not loaded.
 * Existing ModelCapabilities are checked locally: system instructions are required;
 * tool support is required if the filtered registry is non-empty. No new hard model
 * requirements are inferred from ModelPolicy preferences.
 * max_steps caps model turns (initial turn plus tool continuations), not tool count;
 * task max_steps may tighten, never enlarge, the profile cap. read_only exposes
 * only tools classified exclusively as read_file.
 * Network permissions classify tool operations, not the explicitly injected model
 * transport. Permissions do not replace the host's filesystem/network sandbox.
 * Calls to filtered/unknown tools receive normal ToolRegistry errors; the model may
 * recover and complete the task. Declined approvals produce permission_denied;
 * missing approval handlers produce needs_confirmation (no resumable state yet).
 *
 * execute() is synchronous; overlapping/reentrant executions return runtime_busy.
 * cancel_active_request() may be called concurrently or from callbacks. The caller
 * must keep this object alive until execute() and cancellation calls return.
 */
class AgentRuntime {
public:
    /// No discovery or network calls; dependencies are checked on execute().
    AgentRuntime(std::unique_ptr<IModelProvider> provider, std::string api_key,
                 std::string model, std::vector<AgentToolBinding> tools = {});
    ~AgentRuntime();
    AgentRuntime(const AgentRuntime&) = delete;
    AgentRuntime& operator=(const AgentRuntime&) = delete;

    /// Uses context.profile and context.task as the single source of execution inputs.
    /// Existing streaming/progress and confirmation callbacks are forwarded.
    [[nodiscard]] AgentResult execute(const AgentContext& context,
                                      const StreamCallbacks& callbacks = {},
                                      const ConfirmationFn& confirm = {});
    /// No-op when idle; forwards to the active AgentSession and its cancellation flag.
    void cancel_active_request();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace arn::core
