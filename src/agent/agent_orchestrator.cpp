#include "arn/core/agent/agent_orchestrator.hpp"

#include <array>
#include <utility>

namespace arn::core {
namespace {

struct StageDefinition {
    std::string_view profile_id;
    ArtifactType output_type;
};

constexpr std::array<StageDefinition, 4> kStages{{
    {standard_agents::explorer_id, ArtifactType::exploration_report},
    {standard_agents::planner_id, ArtifactType::implementation_plan},
    {standard_agents::coder_id, ArtifactType::change_summary},
    {standard_agents::reviewer_id, ArtifactType::review_report},
}};

OrchestrationResult preflight_failure(std::string code, std::string message) {
    OrchestrationResult result;
    result.status = OrchestrationStatus::failed;
    result.summary = message;
    result.error = OrchestrationError{std::move(code), std::move(message), {}};
    return result;
}

AgentResult agent_failure(std::string code, std::string message) {
    return {AgentStatus::failed, message, {}, AgentError{std::move(code), std::move(message)}};
}

OrchestrationStatus orchestration_status(AgentStatus status) {
    switch (status) {
    case AgentStatus::completed: return OrchestrationStatus::completed;
    case AgentStatus::failed: return OrchestrationStatus::failed;
    case AgentStatus::cancelled: return OrchestrationStatus::cancelled;
    case AgentStatus::needs_input: return OrchestrationStatus::needs_input;
    case AgentStatus::needs_confirmation: return OrchestrationStatus::needs_confirmation;
    }
    return OrchestrationStatus::failed;
}

ContextArtifact user_request(const OrchestrationTask& task) {
    return {ArtifactType::user_request, "user", task.objective};
}

std::vector<ContextArtifact> artifacts_for_stage(
    std::string_view profile_id,
    const std::vector<ContextArtifact>& initial,
    const std::vector<ContextArtifact>& produced) {
    std::vector<ContextArtifact> scoped;
    if (profile_id == standard_agents::explorer_id || profile_id == standard_agents::planner_id)
        scoped = initial;

    for (const auto& artifact : produced) {
        const bool include =
            (profile_id == standard_agents::planner_id
             && artifact.type == ArtifactType::exploration_report)
            || (profile_id == standard_agents::coder_id
                && (artifact.type == ArtifactType::exploration_report
                    || artifact.type == ArtifactType::implementation_plan))
            || (profile_id == standard_agents::reviewer_id
                && (artifact.type == ArtifactType::implementation_plan
                    || artifact.type == ArtifactType::change_summary
                    || artifact.type == ArtifactType::test_result));
        if (include) scoped.push_back(artifact);
    }

    if (profile_id == standard_agents::reviewer_id) {
        for (const auto& artifact : initial)
            if (artifact.type == ArtifactType::test_result) scoped.push_back(artifact);
    }
    return scoped;
}

} // namespace

bool OrchestrationTask::valid() const {
    return id.find_first_not_of(" \t\r\n") != std::string::npos
        && objective.find_first_not_of(" \t\r\n") != std::string::npos
        && !working_directory.empty() && max_agent_runs > 0
        && (!constraints.max_steps || *constraints.max_steps > 0);
}

AgentOrchestrator::AgentOrchestrator(AgentRegistry registry, AgentRuntimeFactory runtime_factory)
    : registry_(std::move(registry)), runtime_factory_(std::move(runtime_factory)) {}

AgentOrchestrator::~AgentOrchestrator() = default;

OrchestrationResult AgentOrchestrator::execute(const OrchestrationTask& task,
                                               const OrchestrationCallbacks& callbacks,
                                               const ConfirmationFn& confirm) {
    if (executing_.exchange(true))
        return preflight_failure("orchestrator_busy", "An orchestration workflow is already running.");
    struct ExecutionGuard {
        std::atomic_bool& executing;
        ~ExecutionGuard() { executing.store(false); }
    } execution_guard{executing_};
    cancel_requested_.store(false);

    if (!task.valid())
        return preflight_failure("invalid_orchestration_task", "Orchestration requires an ID, objective, working directory and positive budgets.");
    if (!runtime_factory_)
        return preflight_failure("missing_runtime_factory", "An agent runtime factory is required.");
    if (task.max_agent_runs < kStages.size())
        return preflight_failure("agent_run_budget_exhausted", "The workflow requires four agent runs.");

    std::array<AgentProfile, kStages.size()> profiles;
    for (std::size_t index = 0; index < kStages.size(); ++index) {
        const auto profile = registry_.find(kStages[index].profile_id);
        if (!profile)
            return preflight_failure("missing_standard_profile",
                                     "A required standard agent profile is missing: "
                                         + std::string(kStages[index].profile_id));
        profiles[index] = *profile;
    }

    OrchestrationResult workflow;
    const ContextArtifact request = user_request(task);

    for (std::size_t index = 0; index < kStages.size(); ++index) {
        const auto& stage = kStages[index];
        if (cancel_requested_.load()) {
            workflow.status = OrchestrationStatus::cancelled;
            workflow.summary = "Workflow cancelled.";
            return workflow;
        }

        std::unique_ptr<IAgentRuntime> runtime;
        try {
            runtime = runtime_factory_(profiles[index]);
        } catch (...) {
            workflow.status = OrchestrationStatus::failed;
            workflow.summary = "Agent runtime construction failed.";
            workflow.error = OrchestrationError{"runtime_factory_failed", workflow.summary,
                                                std::string(stage.profile_id)};
            return workflow;
        }
        if (!runtime) {
            workflow.status = OrchestrationStatus::failed;
            workflow.summary = "Agent runtime factory returned no runtime.";
            workflow.error = OrchestrationError{"runtime_factory_failed", workflow.summary,
                                                std::string(stage.profile_id)};
            return workflow;
        }

        AgentContext context;
        context.profile = profiles[index];
        context.task.id = task.id + ":" + std::string(stage.profile_id);
        context.task.objective = task.objective;
        context.task.working_directory = task.working_directory;
        context.task.inputs = {request};
        context.task.constraints = task.constraints;
        context.artifacts = artifacts_for_stage(stage.profile_id, task.initial_artifacts,
                                                workflow.artifacts);

        {
            std::lock_guard lock(active_mutex_);
            active_runtime_ = runtime.get();
        }

        AgentResult result;
        try {
            if (callbacks.on_stage_started) callbacks.on_stage_started(stage.profile_id);
            if (cancel_requested_.load()) {
                std::lock_guard lock(active_mutex_);
                active_runtime_ = nullptr;
                workflow.status = OrchestrationStatus::cancelled;
                workflow.summary = "Workflow cancelled.";
                return workflow;
            }
            result = runtime->execute(context, callbacks.runtime, confirm);
        } catch (...) {
            result.status = AgentStatus::failed;
            result.summary = "Agent runtime or orchestration callback failed.";
            result.error = AgentError{"orchestration_exception", result.summary};
        }
        {
            std::lock_guard lock(active_mutex_);
            active_runtime_ = nullptr;
        }

        workflow.executions.push_back({std::string(stage.profile_id), result});
        if (callbacks.on_stage_finished) {
            try {
                callbacks.on_stage_finished(stage.profile_id, workflow.executions.back().result);
            } catch (...) {
                workflow.status = OrchestrationStatus::failed;
                workflow.summary = "Orchestration stage callback failed.";
                workflow.error = OrchestrationError{"orchestration_callback_failed", workflow.summary,
                                                    std::string(stage.profile_id)};
                return workflow;
            }
        }

        if (cancel_requested_.load() || result.status == AgentStatus::cancelled) {
            workflow.status = OrchestrationStatus::cancelled;
            workflow.summary = result.summary.empty() ? "Workflow cancelled." : result.summary;
            return workflow;
        }
        if (result.status != AgentStatus::completed) {
            workflow.status = orchestration_status(result.status);
            workflow.summary = result.summary;
            const AgentError error = result.error.value_or(AgentError{"stage_incomplete", result.summary});
            workflow.error = OrchestrationError{error.code, error.message, std::string(stage.profile_id)};
            return workflow;
        }

        const auto stage_artifact_index = workflow.artifacts.size();
        workflow.artifacts.push_back({stage.output_type, std::string(stage.profile_id), result.summary});
        for (const auto& artifact : result.artifacts)
            if (artifact.type == ArtifactType::test_result) workflow.artifacts.push_back(artifact);

        if (index + 1 < kStages.size() && callbacks.before_next_stage) {
            ContinuationDecision decision{ContinuationDecision::proceed};
            try {
                decision = callbacks.before_next_stage(stage.profile_id, result,
                                                       workflow.artifacts[stage_artifact_index]);
            } catch (...) {
                workflow.status = OrchestrationStatus::failed;
                workflow.summary = "Orchestration continuation callback failed.";
                workflow.error = OrchestrationError{"orchestration_callback_failed",
                                                    workflow.summary,
                                                    std::string(stage.profile_id)};
                return workflow;
            }
            if (cancel_requested_.load() || decision == ContinuationDecision::cancel) {
                workflow.status = OrchestrationStatus::cancelled;
                workflow.summary = "Workflow cancelled.";
                return workflow;
            }
            if (decision == ContinuationDecision::decline) {
                workflow.status = OrchestrationStatus::continuation_declined;
                workflow.summary = "Workflow continuation declined.";
                return workflow;
            }
        }
    }

    workflow.status = OrchestrationStatus::completed;
    workflow.summary = workflow.executions.back().result.summary;
    return workflow;
}

AgentResult AgentOrchestrator::execute_agent(std::string_view profile_id,
                                             const AgentTask& task,
                                             const StreamCallbacks& callbacks,
                                             const ConfirmationFn& confirm) {
    if (executing_.exchange(true))
        return agent_failure("orchestrator_busy", "An agent execution is already running.");
    struct ExecutionGuard {
        std::atomic_bool& executing;
        ~ExecutionGuard() { executing.store(false); }
    } execution_guard{executing_};
    cancel_requested_.store(false);

    if (!task.valid())
        return agent_failure("invalid_agent_task",
                             "Agent execution requires an ID, objective, working directory and positive budget.");
    if (!runtime_factory_)
        return agent_failure("missing_runtime_factory", "An agent runtime factory is required.");
    const auto profile = registry_.find(profile_id);
    if (!profile)
        return agent_failure("unknown_agent_profile",
                             "The requested agent profile is not registered.");

    std::unique_ptr<IAgentRuntime> runtime;
    try {
        runtime = runtime_factory_(*profile);
    } catch (...) {
        return agent_failure("runtime_factory_failed", "Agent runtime construction failed.");
    }
    if (!runtime)
        return agent_failure("runtime_factory_failed", "Agent runtime factory returned no runtime.");

    AgentContext context;
    context.profile = *profile;
    context.task = task;
    {
        std::lock_guard lock(active_mutex_);
        active_runtime_ = runtime.get();
    }

    AgentResult result;
    try {
        if (cancel_requested_.load()) {
            result = {AgentStatus::cancelled, "Execution cancelled.", {}, std::nullopt};
        } else {
            result = runtime->execute(context, callbacks, confirm);
        }
    } catch (...) {
        result = agent_failure("execution_exception", "Agent runtime execution failed.");
    }
    {
        std::lock_guard lock(active_mutex_);
        active_runtime_ = nullptr;
    }
    if (cancel_requested_.load() || result.status == AgentStatus::cancelled)
        return {AgentStatus::cancelled,
                result.summary.empty() ? "Execution cancelled." : result.summary,
                std::move(result.artifacts), std::nullopt};
    return result;
}

void AgentOrchestrator::cancel_active_execution() {
    cancel_requested_.store(true);
    std::lock_guard lock(active_mutex_);
    if (active_runtime_) active_runtime_->cancel_active_request();
}

void AgentOrchestrator::cancel_active_workflow() {
    cancel_active_execution();
}

} // namespace arn::core
