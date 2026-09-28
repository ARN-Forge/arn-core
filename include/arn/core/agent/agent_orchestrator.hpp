/** @file agent_orchestrator.hpp
 * @brief Deterministic sequential coordination of independent agent runtimes.
 */
#pragma once

#include "arn/core/agent/agent_registry.hpp"
#include "arn/core/agent/agent_runtime.hpp"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace arn::core {

/** @brief Input for the fixed Explorer -> Planner -> Coder -> Reviewer workflow. */
struct OrchestrationTask {
    std::string id;
    std::string objective;
    std::filesystem::path working_directory;
    std::vector<ContextArtifact> initial_artifacts;
    TaskConstraints constraints;
    /// Maximum runtime instances that may execute. Four permits the full v1 workflow.
    std::size_t max_agent_runs{4};

    [[nodiscard]] bool valid() const;
};

enum class OrchestrationStatus {
    completed,
    failed,
    cancelled,
    continuation_declined,
    needs_input,
    needs_confirmation
};

/** @brief Host decision made after a completed stage and before the next stage. */
enum class ContinuationDecision { proceed, decline, cancel };

struct AgentExecutionRecord {
    std::string profile_id;
    AgentResult result;
};

struct OrchestrationError {
    std::string code;
    std::string message;
    /// Empty only for errors detected before a stage starts.
    std::string profile_id;
};

struct OrchestrationResult {
    OrchestrationStatus status{OrchestrationStatus::failed};
    std::string summary;
    std::vector<AgentExecutionRecord> executions;
    /// Semantic stage outputs, never private provider conversation history.
    std::vector<ContextArtifact> artifacts;
    std::optional<OrchestrationError> error;
};

struct OrchestrationCallbacks {
    std::function<void(std::string_view profile_id)> on_stage_started;
    std::function<void(std::string_view profile_id, const AgentResult& result)> on_stage_finished;
    /** Called only between successful stages. The artifact is the exact semantic
     * stage output that will be supplied to downstream contexts if execution proceeds.
     */
    std::function<ContinuationDecision(std::string_view profile_id,
                                       const AgentResult& result,
                                       const ContextArtifact& artifact)> before_next_stage;
    /// Forwarded to each active AgentRuntime. Stage callbacks identify its owner.
    StreamCallbacks runtime;
};

using AgentRuntimeFactory =
    std::function<std::unique_ptr<IAgentRuntime>(const AgentProfile& profile)>;

/** @brief Coordinates the fixed four-stage coding workflow through AgentRuntime.
 *
 * Every stage receives a fresh AgentContext and a separately created runtime.
 * Context crosses stages only through scoped ContextArtifact values: Explorer
 * receives initial artifacts; Planner receives those plus ExplorationReport;
 * Coder receives ExplorationReport and ImplementationPlan; Reviewer receives
 * ImplementationPlan, ChangeSummary, and relevant TestResult artifacts.
 * The original UserRequest is an explicit input to every stage.
 *
 * Mandatory stage failure or intervention stops later stages while retaining
 * completed records. Cancellation is forwarded to the active runtime and also
 * prevents the next stage from starting. AgentRuntime still executes one agent;
 * AgentSession still owns each model/tool conversation.
 */
class AgentOrchestrator {
public:
    AgentOrchestrator(AgentRegistry registry, AgentRuntimeFactory runtime_factory);
    ~AgentOrchestrator();
    AgentOrchestrator(const AgentOrchestrator&) = delete;
    AgentOrchestrator& operator=(const AgentOrchestrator&) = delete;

    [[nodiscard]] OrchestrationResult execute(const OrchestrationTask& task,
                                              const OrchestrationCallbacks& callbacks = {},
                                              const ConfirmationFn& confirm = {});
    /** Executes exactly one registered profile through a fresh AgentRuntime. */
    [[nodiscard]] AgentResult execute_agent(std::string_view profile_id,
                                            const AgentTask& task,
                                            const StreamCallbacks& callbacks = {},
                                            const ConfirmationFn& confirm = {});
    void cancel_active_execution();
    void cancel_active_workflow();

private:
    AgentRegistry registry_;
    AgentRuntimeFactory runtime_factory_;
    std::atomic_bool executing_{false};
    std::atomic_bool cancel_requested_{false};
    std::mutex active_mutex_;
    IAgentRuntime* active_runtime_{nullptr};
};

} // namespace arn::core
