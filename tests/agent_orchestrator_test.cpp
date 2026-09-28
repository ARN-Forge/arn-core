#include "arn/core/agent/agent_orchestrator.hpp"

#include <algorithm>
#include <condition_variable>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace arn::core;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct StageScript {
    AgentStatus status{AgentStatus::completed};
    std::string summary;
    std::optional<AgentError> error;
    std::vector<ContextArtifact> artifacts;
    bool block_until_cancelled{false};
};

struct FakeState {
    std::mutex mutex;
    std::condition_variable cv;
    std::map<std::string, StageScript, std::less<>> scripts;
    std::vector<std::string> created;
    std::vector<std::size_t> instance_ids;
    std::vector<AgentContext> contexts;
    std::size_t next_instance_id{1};
    std::size_t cancel_calls{0};
    bool blocking_stage_entered{false};
    bool confirmation_handler_seen{false};
    bool runtime_callbacks_seen{false};
};

class FakeRuntime final : public IAgentRuntime {
public:
    FakeRuntime(std::shared_ptr<FakeState> state, std::string profile_id, std::size_t instance_id)
        : state_(std::move(state)), profile_id_(std::move(profile_id)), instance_id_(instance_id) {}

    AgentResult execute(const AgentContext& context, const StreamCallbacks& callbacks,
                        const ConfirmationFn& confirm) override {
        StageScript script;
        {
            std::lock_guard lock(state_->mutex);
            state_->instance_ids.push_back(instance_id_);
            state_->contexts.push_back(context);
            state_->confirmation_handler_seen |= static_cast<bool>(confirm);
            state_->runtime_callbacks_seen |= static_cast<bool>(callbacks.on_text)
                || static_cast<bool>(callbacks.on_progress);
            script = state_->scripts.at(profile_id_);
            if (script.block_until_cancelled) {
                state_->blocking_stage_entered = true;
                state_->cv.notify_all();
            }
        }

        if (script.block_until_cancelled) {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return cancelled_; });
            return {AgentStatus::cancelled, "Active stage cancelled.", {}, std::nullopt};
        }
        return {script.status, script.summary, script.artifacts, script.error};
    }

    void cancel_active_request() override {
        {
            std::lock_guard lock(mutex_);
            cancelled_ = true;
        }
        {
            std::lock_guard lock(state_->mutex);
            ++state_->cancel_calls;
        }
        cv_.notify_all();
    }

private:
    std::shared_ptr<FakeState> state_;
    std::string profile_id_;
    std::size_t instance_id_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool cancelled_{false};
};

std::shared_ptr<FakeState> default_state() {
    auto state = std::make_shared<FakeState>();
    state->scripts.emplace("explorer", StageScript{AgentStatus::completed, "exploration"});
    state->scripts.emplace("planner", StageScript{AgentStatus::completed, "plan"});
    state->scripts.emplace("coder", StageScript{AgentStatus::completed, "changes"});
    state->scripts.emplace("reviewer", StageScript{AgentStatus::completed, "review"});
    return state;
}

AgentRuntimeFactory factory_for(const std::shared_ptr<FakeState>& state) {
    return [state](const AgentProfile& profile) -> std::unique_ptr<IAgentRuntime> {
        std::lock_guard lock(state->mutex);
        state->created.push_back(profile.id);
        const auto instance_id = state->next_instance_id++;
        return std::make_unique<FakeRuntime>(state, profile.id, instance_id);
    };
}

OrchestrationTask task(std::string id = "workflow-1", std::string objective = "Implement feature") {
    OrchestrationTask value;
    value.id = std::move(id);
    value.objective = std::move(objective);
    value.working_directory = "virtual-workspace";
    value.initial_artifacts = {
        {ArtifactType::generic, "host", "constraint"},
        {ArtifactType::test_result, "host", "initial tests"},
    };
    return value;
}

std::vector<std::string> expected_order() {
    return {"explorer", "planner", "coder", "reviewer"};
}

bool has_artifact(const std::vector<ContextArtifact>& artifacts, ArtifactType type,
                  std::string_view content) {
    return std::any_of(artifacts.begin(), artifacts.end(), [&](const ContextArtifact& artifact) {
        return artifact.type == type && artifact.content == content;
    });
}

void happy_path_and_handoffs() {
    auto state = default_state();
    state->scripts["coder"].artifacts.push_back(
        {ArtifactType::test_result, "coder", "coder tests"});
    AgentOrchestrator orchestrator{standard_agents::make_registry(), factory_for(state)};
    std::vector<std::string> started;
    std::vector<std::string> finished;
    OrchestrationCallbacks callbacks;
    callbacks.on_stage_started = [&](std::string_view id) { started.emplace_back(id); };
    callbacks.on_stage_finished = [&](std::string_view id, const AgentResult&) {
        finished.emplace_back(id);
    };
    callbacks.runtime.on_text = [](std::string_view) {};
    const auto result = orchestrator.execute(task(), callbacks,
        [](const ConfirmationRequest&) { return true; });

    check(result.status == OrchestrationStatus::completed && result.summary == "review",
          "Happy path completes with reviewer summary");
    check(state->created == expected_order(), "Exactly four runtimes created in standard order");
    check(started == expected_order() && finished == expected_order(), "Stage observability order");
    check(result.executions.size() == 4, "Four execution records retained");
    for (std::size_t index = 0; index < result.executions.size(); ++index)
        check(result.executions[index].profile_id == expected_order()[index], "Execution record order");
    check(state->contexts.size() == 4 && state->instance_ids.size() == 4,
          "Fresh context and runtime per stage");
    check(std::vector<std::size_t>(state->instance_ids) == std::vector<std::size_t>{1, 2, 3, 4},
          "Runtime instances are isolated");
    check(state->confirmation_handler_seen && state->runtime_callbacks_seen,
          "Confirmation and runtime callbacks forwarded");

    for (std::size_t index = 0; index < state->contexts.size(); ++index) {
        const auto& context = state->contexts[index];
        check(context.profile.id == expected_order()[index], "Correct profile assigned to stage");
        check(context.task.objective == "Implement feature", "Original objective reaches every stage");
        check(context.task.inputs.size() == 1
                  && context.task.inputs[0].type == ArtifactType::user_request
                  && context.task.inputs[0].content == "Implement feature",
              "Explicit UserRequest reaches every stage");
    }

    check(has_artifact(state->contexts[0].artifacts, ArtifactType::generic, "constraint"),
          "Explorer receives initial artifacts");
    check(has_artifact(state->contexts[1].artifacts, ArtifactType::exploration_report, "exploration"),
          "Explorer output reaches Planner");
    check(has_artifact(state->contexts[2].artifacts, ArtifactType::exploration_report, "exploration")
              && has_artifact(state->contexts[2].artifacts, ArtifactType::implementation_plan, "plan"),
          "Coder receives exploration and plan");
    check(!has_artifact(state->contexts[2].artifacts, ArtifactType::generic, "constraint"),
          "Coder does not receive unrelated initial artifacts");
    check(has_artifact(state->contexts[3].artifacts, ArtifactType::implementation_plan, "plan")
              && has_artifact(state->contexts[3].artifacts, ArtifactType::change_summary, "changes")
              && has_artifact(state->contexts[3].artifacts, ArtifactType::test_result, "coder tests")
              && has_artifact(state->contexts[3].artifacts, ArtifactType::test_result, "initial tests"),
          "Reviewer receives scoped plan, change and test artifacts");
    check(!has_artifact(state->contexts[3].artifacts, ArtifactType::exploration_report, "exploration"),
          "Reviewer does not receive unrelated exploration artifact");
    check(has_artifact(result.artifacts, ArtifactType::exploration_report, "exploration")
              && has_artifact(result.artifacts, ArtifactType::implementation_plan, "plan")
              && has_artifact(result.artifacts, ArtifactType::change_summary, "changes")
              && has_artifact(result.artifacts, ArtifactType::review_report, "review"),
          "Final result exposes semantic stage artifacts");

    check(state->contexts[0].profile.permissions.decision_for(OperationClass::write_file)
              == PermissionDecision::deny
              && state->contexts[1].profile.permissions.decision_for(OperationClass::write_file)
                     == PermissionDecision::deny
              && state->contexts[2].profile.permissions.decision_for(OperationClass::write_file)
                     == PermissionDecision::ask_user
              && state->contexts[3].profile.permissions.decision_for(OperationClass::write_file)
                     == PermissionDecision::deny,
          "Orchestration does not broaden profile permissions");
}

void mandatory_failures_stop_pipeline() {
    const auto order = expected_order();
    for (std::size_t failed_index = 0; failed_index < order.size(); ++failed_index) {
        auto state = default_state();
        state->scripts[order[failed_index]] = {
            AgentStatus::failed, "stage failed", AgentError{"offline_failure", "stage failed"}};
        AgentOrchestrator orchestrator{standard_agents::make_registry(), factory_for(state)};
        const auto result = orchestrator.execute(task());
        check(result.status == OrchestrationStatus::failed, "Mandatory failure propagated");
        check(result.executions.size() == failed_index + 1, "Later stages do not run after failure");
        check(state->created.size() == failed_index + 1, "No later runtime constructed after failure");
        check(result.error && result.error->profile_id == order[failed_index]
                  && result.error->code == "offline_failure",
              "Failure stage and diagnostic retained");
        if (failed_index == 3)
            check(result.executions.size() == 4 && result.artifacts.size() >= 3,
                  "Reviewer failure preserves earlier records and artifacts");
    }
}

void intervention_statuses_propagate() {
    for (const auto status : {AgentStatus::needs_input, AgentStatus::needs_confirmation}) {
        auto state = default_state();
        state->scripts["planner"] = {status, "human action required",
                                      AgentError{"human_action", "human action required"}};
        AgentOrchestrator orchestrator{standard_agents::make_registry(), factory_for(state)};
        const auto result = orchestrator.execute(task());
        const auto expected = status == AgentStatus::needs_input
            ? OrchestrationStatus::needs_input : OrchestrationStatus::needs_confirmation;
        check(result.status == expected, "Intervention status preserved");
        check(result.executions.size() == 2 && state->created.size() == 2,
              "Intervention stops later stages");
        check(result.error && result.error->profile_id == "planner", "Intervention stage retained");
    }
}

void budget_and_missing_profile_fail_before_execution() {
    auto state = default_state();
    AgentOrchestrator budgeted{standard_agents::make_registry(), factory_for(state)};
    auto limited = task();
    limited.max_agent_runs = 3;
    const auto budget_result = budgeted.execute(limited);
    check(budget_result.status == OrchestrationStatus::failed
              && budget_result.error && budget_result.error->code == "agent_run_budget_exhausted",
          "Insufficient workflow budget fails deterministically");
    check(state->created.empty(), "Budget checked before runtime construction");

    AgentRegistry incomplete;
    check(incomplete.register_profile(standard_agents::explorer()), "Register Explorer");
    check(incomplete.register_profile(standard_agents::planner()), "Register Planner");
    check(incomplete.register_profile(standard_agents::coder()), "Register Coder");
    AgentOrchestrator missing{std::move(incomplete), factory_for(state)};
    const auto missing_result = missing.execute(task());
    check(missing_result.status == OrchestrationStatus::failed
              && missing_result.error && missing_result.error->code == "missing_standard_profile",
          "Missing standard profile rejected");
    check(state->created.empty(), "Missing profile fails before unsafe execution");
}

void cancellation_during_active_stage() {
    auto state = default_state();
    state->scripts["explorer"].block_until_cancelled = true;
    AgentOrchestrator orchestrator{standard_agents::make_registry(), factory_for(state)};
    auto future = std::async(std::launch::async, [&] { return orchestrator.execute(task()); });
    {
        std::unique_lock lock(state->mutex);
        state->cv.wait(lock, [&] { return state->blocking_stage_entered; });
    }
    orchestrator.cancel_active_workflow();
    const auto result = future.get();
    check(result.status == OrchestrationStatus::cancelled, "Active cancellation propagated");
    check(result.executions.size() == 1 && state->created == std::vector<std::string>{"explorer"},
          "Active cancellation prevents later stages and preserves record");
    check(state->cancel_calls == 1, "Cancellation forwarded to active runtime exactly once");
}

void cancellation_between_stages() {
    auto state = default_state();
    AgentOrchestrator orchestrator{standard_agents::make_registry(), factory_for(state)};
    OrchestrationCallbacks callbacks;
    callbacks.on_stage_finished = [&](std::string_view id, const AgentResult&) {
        if (id == "explorer") orchestrator.cancel_active_workflow();
    };
    const auto result = orchestrator.execute(task(), callbacks);
    check(result.status == OrchestrationStatus::cancelled, "Between-stage cancellation propagated");
    check(result.executions.size() == 1 && state->created == std::vector<std::string>{"explorer"},
          "Between-stage cancellation prevents Planner");
}

void continuation_control_preserves_plan_and_records() {
    {
        auto state = default_state();
        AgentOrchestrator orchestrator{standard_agents::make_registry(), factory_for(state)};
        std::vector<std::string> checkpoints;
        std::string planner_artifact;
        OrchestrationCallbacks callbacks;
        callbacks.before_next_stage = [&](std::string_view id, const AgentResult& result,
                                          const ContextArtifact& artifact) {
            checkpoints.emplace_back(id);
            check(result.summary == artifact.content,
                  "Continuation receives the semantic stage result");
            if (id == standard_agents::planner_id) {
                check(artifact.type == ArtifactType::implementation_plan,
                      "Planner checkpoint receives implementation plan artifact");
                planner_artifact = artifact.content;
                return ContinuationDecision::decline;
            }
            return ContinuationDecision::proceed;
        };
        const auto result = orchestrator.execute(task(), callbacks);
        check(result.status == OrchestrationStatus::continuation_declined,
              "Host decline has a dedicated status");
        check(state->created == std::vector<std::string>{"explorer", "planner"},
              "Host decline prevents Coder and Reviewer");
        check(result.executions.size() == 2 && planner_artifact == "plan",
              "Completed records and real Planner artifact are retained");
        check(checkpoints == std::vector<std::string>{"explorer", "planner"},
              "Continuation callback runs only between completed stages");
    }
    {
        auto state = default_state();
        AgentOrchestrator orchestrator{standard_agents::make_registry(), factory_for(state)};
        OrchestrationCallbacks callbacks;
        callbacks.before_next_stage = [&](std::string_view id, const AgentResult&,
                                          const ContextArtifact&) {
            return id == standard_agents::planner_id
                ? ContinuationDecision::cancel : ContinuationDecision::proceed;
        };
        const auto result = orchestrator.execute(task(), callbacks);
        check(result.status == OrchestrationStatus::cancelled,
              "Checkpoint cancellation remains distinct from decline");
        check(result.executions.size() == 2
                  && state->created == std::vector<std::string>{"explorer", "planner"},
              "Checkpoint cancellation preserves completed records and stops later stages");
    }
}

void direct_registered_agent_execution() {
    auto state = default_state();
    AgentOrchestrator orchestrator{standard_agents::make_registry(), factory_for(state)};
    const auto order = expected_order();
    for (const auto& id : order) {
        AgentTask direct_task;
        direct_task.id = "direct:" + id;
        direct_task.objective = "Run one profile";
        direct_task.working_directory = "virtual-workspace";
        const auto result = orchestrator.execute_agent(id, direct_task);
        check(result.status == AgentStatus::completed,
              "Direct registered agent completes with AgentResult semantics");
    }
    check(state->created == order, "Each direct call creates only the requested runtime");
    check(state->contexts.size() == 4, "Direct calls create four isolated contexts");
    for (std::size_t index = 0; index < order.size(); ++index)
        check(state->contexts[index].profile.id == order[index],
              "Direct execution uses the selected registry profile");

    AgentTask valid;
    valid.id = "direct:missing";
    valid.objective = "Missing profile";
    valid.working_directory = "virtual-workspace";
    const auto missing = orchestrator.execute_agent("missing", valid);
    check(missing.status == AgentStatus::failed && missing.error
              && missing.error->code == "unknown_agent_profile",
          "Unknown direct profile is rejected before runtime creation");
}

void sequential_workflows_are_isolated() {
    auto state = default_state();
    AgentOrchestrator orchestrator{standard_agents::make_registry(), factory_for(state)};
    const auto first = orchestrator.execute(task("first", "First objective"));
    const auto second = orchestrator.execute(task("second", "Second objective"));
    check(first.status == OrchestrationStatus::completed
              && second.status == OrchestrationStatus::completed,
          "Sequential workflows complete");
    check(state->created.size() == 8 && state->contexts.size() == 8
              && state->instance_ids == std::vector<std::size_t>{1, 2, 3, 4, 5, 6, 7, 8},
          "Sequential workflows use fresh runtime instances and contexts");
    for (std::size_t index = 0; index < 4; ++index)
        check(state->contexts[index].task.objective == "First objective"
                  && state->contexts[index + 4].task.objective == "Second objective",
              "No objective or private history leaks between workflows");
}

} // namespace

int main() {
    happy_path_and_handoffs();
    mandatory_failures_stop_pipeline();
    intervention_statuses_propagate();
    budget_and_missing_profile_fail_before_execution();
    cancellation_during_active_stage();
    cancellation_between_stages();
    continuation_control_preserves_plan_and_records();
    direct_registered_agent_execution();
    sequential_workflows_are_isolated();
    return 0;
}
