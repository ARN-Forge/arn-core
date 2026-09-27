#include "arn/core/agent/agent_runtime.hpp"
#include <functional>
#include <future>
#include <iostream>
#include <latch>
#include <limits>
#include <stdexcept>

namespace {
using namespace arn::core;
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Existing session tests use a TU-local fake. This fixture records session hooks
// and leaves canned responses intact across reset, without any network transport.
class FakeProvider final : public IModelProvider {
public:
    int starts{0}, continues{0}, discoveries{0}, trims{0}, resets{0}, cancellations{0};
    std::vector<std::string> history;
    std::vector<std::string> start_inputs;
    std::vector<std::size_t> initial_history_sizes;
    std::vector<ToolDefinition> exposed;
    std::vector<ToolResponse> responses;
    std::string system, selected_model;
    std::string requested_tool;
    bool fail{false}, throw_request{false}, repeat_tools{false};
    bool cancel_result{false}, fail_reset{false};
    ModelCapabilities capabilities;
    std::function<void()> on_start;
    std::function<void()> on_cancel;

    ProviderType type() const noexcept override { return ProviderType::custom; }
    std::string_view name() const noexcept override { return "Offline fake"; }
    std::string preferred_model(const std::vector<std::string>&) const override { return "unused"; }
    ModelCapabilities model_capabilities(std::string_view) const override { return capabilities; }
    ApiResult list_models(const std::string&, const std::atomic_bool*) override {
        ++discoveries;
        throw std::runtime_error("Runtime must not discover models");
    }
    void trim_history(std::size_t) override { ++trims; }
    void cancel_active_request() override {
        ++cancellations;
        if (on_cancel) on_cancel();
    }
    void reset_session() override {
        ++resets;
        if (fail_reset) throw std::runtime_error("Reset failed");
        history.clear();
    }
    std::size_t session_entries() const noexcept override { return history.size(); }
    ModelTurn tool_turn() {
        return {.ok = true, .tool_calls = {{"call-1", requested_tool, {{"value", 7}}}}};
    }
    ModelTurn start_turn(const std::string&, const std::string& model,
                        const std::string& instruction, const std::string& input,
                        const ToolRegistry& tools, const StreamCallbacks& callbacks,
                        const std::atomic_bool*) override {
        ++starts;
        initial_history_sizes.push_back(history.size());
        history.push_back(input);
        start_inputs.push_back(input);
        system = instruction;
        selected_model = model;
        exposed = tools.definitions();
        if (on_start) on_start();
        if (throw_request) throw std::runtime_error("Must not expose exception internals");
        if (cancel_result) return {.ok = false, .error_message = "Cancelled", .cancelled = true};
        if (fail) return {.ok = false, .error_message = "Offline failure"};
        if (!requested_tool.empty()) return tool_turn();
        if (callbacks.on_text) callbacks.on_text("answer");
        return {.ok = true, .text = "answer"};
    }
    ModelTurn continue_turn(const std::string&, const std::string&, const std::string&,
                           const std::vector<ToolResponse>& values, const ToolRegistry&,
                           const StreamCallbacks& callbacks, const std::atomic_bool*) override {
        ++continues;
        responses = values;
        history.push_back("tool response");
        if (repeat_tools) return tool_turn();
        if (callbacks.on_text) callbacks.on_text("tool answer");
        return {.ok = true, .text = "tool answer"};
    }
};

class CountingTool final : public ITool {
public:
    int calls{0};
    bool ask{false};
    ToolDefinition def{"sample", "Offline counting tool", {{"type", "object"}}};
    const ToolDefinition& definition() const noexcept override { return def; }
    bool requires_confirmation() const noexcept override { return ask; }
    ToolResult execute(const nlohmann::json& arguments, const ToolContext&) override {
        ++calls;
        return ToolResult::success({{"seen", arguments.at("value")}});
    }
};

AgentContext context() {
    AgentContext c;
    c.profile.id = "test-agent";
    c.profile.display_name = "Test Agent";
    c.profile.instructions = "Explain carefully";
    c.profile.default_skills = {"metadata-only"};
    c.task.id = "task-1";
    c.task.objective = "Task input";
    c.task.working_directory = "virtual-workspace";
    c.task.inputs = {{ArtifactType::user_request, "caller", "input-artifact"}};
    c.artifacts = {{ArtifactType::exploration_report, "previous-agent", "local-artifact"}};
    return c;
}

void basic_and_isolation() {
    auto provider = std::make_unique<FakeProvider>();
    auto* fake = provider.get();
    fake->history.push_back("old private history");
    AgentRuntime runtime{std::move(provider), "offline-placeholder", "host-selected"};
    auto c = context();
    c.profile.model_policy = {ModelCapability::coding, ModelCapability::fast};
    std::string streamed;
    auto result = runtime.execute(c, {.on_text = [&](std::string_view text) { streamed += text; }});
    check(result.status == AgentStatus::completed && result.summary == "answer", "Successful result");
    check(result.artifacts.size() == 1 && result.artifacts[0].producer == c.profile.id, "Output provenance");
    check(fake->trims == 1 && fake->starts == 1 && fake->resets == 2, "Uses AgentSession lifecycle");
    check(fake->system.find(c.profile.instructions) != std::string::npos
          && fake->system.find(c.profile.display_name) != std::string::npos
          && fake->system.find(c.profile.id) != std::string::npos, "Profile translation");
    const auto input = nlohmann::json::parse(fake->start_inputs[0]);
    check(input.at("objective") == c.task.objective && input.at("working_directory") == "virtual-workspace", "Task translation");
    check(input.at("inputs")[0].at("content") == "input-artifact"
          && input.at("artifacts")[0].at("producer") == "previous-agent", "Artifacts available");
    check(streamed == "answer" && fake->discoveries == 0 && fake->selected_model == "host-selected", "Callbacks and no model routing");
    check(c.artifacts[0].content == "local-artifact" && c.task.inputs[0].content == "input-artifact", "Caller unchanged");
    check(fake->history.empty(), "Private history cleared after execution");
    auto second = context();
    second.task.objective = "Second task";
    second.task.inputs.clear();
    second.artifacts.clear();
    check(runtime.execute(second).status == AgentStatus::completed, "Second execution");
    check(fake->initial_history_sizes == std::vector<std::size_t>{0, 0}, "No history leak");
    check(fake->start_inputs.back().find("local-artifact") == std::string::npos, "No artifact leak");
    check(fake->history.empty() && c.task.objective == "Task input", "Independent contexts");
}

void permissions_and_tools() {
    for (auto decision : {PermissionDecision::deny, PermissionDecision::allow, PermissionDecision::ask_user}) {
        auto provider = std::make_unique<FakeProvider>();
        auto* fake = provider.get();
        fake->requested_tool = "sample";
        auto tool = std::make_shared<CountingTool>();
        AgentRuntime runtime{std::move(provider), "offline-placeholder", "host-selected",
                             {{tool, {OperationClass::write_file}}}};
        auto c = context();
        c.profile.allowed_tools = {"sample"};
        check(c.profile.permissions.set_decision(OperationClass::write_file, decision), "Set policy");
        int confirmations = 0;
        std::string progress;
        const auto result = runtime.execute(c, {.on_progress = [&](std::string_view text) { progress += text; }},
            [&](const ConfirmationRequest& request) {
                ++confirmations;
                check(request.name == "sample" && request.arguments.at("value") == 7, "Confirmation details");
                return true;
            });
        check(result.status == AgentStatus::completed, "Model can finish after tool response");
        check(fake->continues == 1 && fake->responses.size() == 1, "Normal session tool continuation");
        check(progress.find("sample") != std::string::npos, "Tool progress forwarded");
        if (decision == PermissionDecision::deny) {
            check(tool->calls == 0 && fake->exposed.empty(), "Denied tool not exposed or invoked");
            check(fake->responses[0].result.contains("error"), "Invented forbidden call rejected by registry");
        } else {
            check(tool->calls == 1 && fake->exposed.size() == 1, "Permitted tool invoked");
            check(fake->responses[0].result.at("seen") == 7, "Actual tool result returned");
        }
        check(confirmations == (decision == PermissionDecision::ask_user ? 1 : 0), "AskUser enforced");
    }

    // Unknown/unclassified operations, read-only restriction, allowlist and multi-effect tools.
    for (int scenario = 0; scenario < 5; ++scenario) {
        auto provider = std::make_unique<FakeProvider>();
        auto* fake = provider.get();
        fake->requested_tool = "sample";
        auto tool = std::make_shared<CountingTool>();
        std::vector<OperationClass> operations{OperationClass::write_file};
        auto c = context();
        c.profile.allowed_tools = {"sample"};
        check(c.profile.permissions.set_decision(OperationClass::write_file, PermissionDecision::allow), "Allow write");
        if (scenario == 0) operations = {static_cast<OperationClass>(999)};
        if (scenario == 1) operations.clear();
        if (scenario == 2) c.task.constraints.read_only = true;
        if (scenario == 3) c.profile.allowed_tools.clear();
        if (scenario == 4) operations.push_back(OperationClass::network);
        AgentRuntime runtime{std::move(provider), "offline-placeholder", "host-selected", {{tool, operations}}};
        (void)runtime.execute(c);
        check(tool->calls == 0 && fake->exposed.empty(), "Structural restriction cannot be bypassed");
    }

    for (int scenario = 0; scenario < 3; ++scenario) {
        auto provider = std::make_unique<FakeProvider>();
        provider->requested_tool = "sample";
        auto tool = std::make_shared<CountingTool>();
        tool->ask = scenario == 2; // native confirmation requirement survives Allow
        AgentRuntime runtime{std::move(provider), "offline-placeholder", "host-selected", {{tool, {OperationClass::read_file}}}};
        auto c = context();
        c.profile.allowed_tools = {"sample"};
        check(c.profile.permissions.set_decision(OperationClass::read_file,
              scenario == 2 ? PermissionDecision::allow : PermissionDecision::ask_user), "Set confirmation policy");
        ConfirmationFn confirm;
        if (scenario == 1) confirm = [](const ConfirmationRequest&) { return false; };
        auto result = runtime.execute(c, {}, confirm);
        check(tool->calls == 0, "No approval means no side effects");
        check(result.status == (scenario == 1 ? AgentStatus::failed : AgentStatus::needs_confirmation), "Approval result mapping");
    }
}

void failures_and_cancellation() {
    auto provider = std::make_unique<FakeProvider>();
    auto* fake = provider.get();
    AgentRuntime runtime{std::move(provider), "offline-placeholder", "host-selected"};
    auto c = context();
    auto invalid = c;
    invalid.task.objective.clear();
    check(runtime.execute(invalid).error->code == "invalid_task", "Invalid task rejected");
    invalid = c;
    invalid.profile.max_steps = 0;
    check(runtime.execute(invalid).error->code == "invalid_profile", "Invalid profile rejected");
    check(fake->starts == 0 && fake->resets == 0 && fake->discoveries == 0, "Validation does not contact provider");
    fake->fail = true;
    auto result = runtime.execute(c);
    check(result.status == AgentStatus::failed && result.error->message == "Offline failure", "Session error mapped");
    fake->fail = false;
    fake->throw_request = true;
    check(runtime.execute(c).error->code == "execution_exception" && fake->history.empty(), "Exception cleanup");
    fake->throw_request = false;
    fake->on_start = [&] {
        check(runtime.execute(c).error->code == "runtime_busy", "Reentrant execution rejected");
        runtime.cancel_active_request();
    };
    check(runtime.execute(c).status == AgentStatus::cancelled, "Active cancellation mapped");
    check(fake->cancellations == 1 && fake->history.empty(), "Cancellation reaches session/provider");
    fake->on_start = {};
    runtime.cancel_active_request();
    check(fake->cancellations == 1, "Idle cancellation does not affect future task");
    check(runtime.execute(c).status == AgentStatus::completed, "Cancellation does not leak");
    fake->cancel_result = true;
    check(runtime.execute(c).status == AgentStatus::cancelled, "Provider cancellation mapped");
    fake->cancel_result = false;
    fake->fail_reset = true;
    const int starts = fake->starts;
    check(runtime.execute(c).status == AgentStatus::failed && fake->starts == starts, "Reset failure cannot leak old history");
}

void budgets_and_capabilities() {
    auto provider = std::make_unique<FakeProvider>();
    auto* fake = provider.get();
    fake->requested_tool = "sample";
    fake->repeat_tools = true;
    auto tool = std::make_shared<CountingTool>();
    AgentRuntime runtime{std::move(provider), "offline-placeholder", "host-selected", {{tool, {OperationClass::read_file}}}};
    auto c = context();
    c.profile.allowed_tools = {"sample"};
    check(c.profile.permissions.set_decision(OperationClass::read_file, PermissionDecision::allow), "Read permission");
    c.profile.max_steps = 3;
    c.task.constraints.max_steps = 2;
    check(runtime.execute(c).status == AgentStatus::failed, "Budget exhaustion not success");
    check(fake->continues == 1 && tool->calls == 1, "Task cap maps to session tool rounds");
    c.task.constraints.max_steps = 99;
    check(runtime.execute(c).status == AgentStatus::failed, "Profile cap enforced");
    check(fake->continues == 3 && tool->calls == 3, "Task cannot broaden profile cap");
    c.task.constraints.max_steps = 1;
    check(runtime.execute(c).status == AgentStatus::failed && tool->calls == 3, "One step cannot execute tool round");
    fake->capabilities.supports_tools = false;
    const auto starts = fake->starts;
    check(runtime.execute(c).error->code == "model_incompatible" && fake->starts == starts, "Known tool capability incompatibility");
    c.profile.allowed_tools.clear();
    fake->capabilities.supports_system_instruction = false;
    check(runtime.execute(c).error->code == "model_incompatible" && fake->starts == starts, "System instruction capability incompatibility");
}

void approval_cancellation_and_policy_isolation() {
    auto provider = std::make_unique<FakeProvider>();
    auto* fake = provider.get();
    fake->requested_tool = "sample";
    auto tool = std::make_shared<CountingTool>();
    AgentRuntime runtime{std::move(provider), "offline-placeholder", "host-selected", {{tool, {OperationClass::read_file}}}};
    auto c = context();
    c.profile.allowed_tools = {"sample"};
    c.task.constraints.read_only = true;
    check(c.profile.permissions.set_decision(OperationClass::read_file, PermissionDecision::ask_user), "Ask read");
    const auto cancelled = runtime.execute(c, {}, [&](const ConfirmationRequest&) {
        runtime.cancel_active_request();
        return true;
    });
    check(cancelled.status == AgentStatus::cancelled && tool->calls == 0 && fake->continues == 0,
          "Cancellation during approval prevents tool and continuation");
    check(fake->cancellations == 1, "Approval cancellation reaches provider");
    const auto approved = runtime.execute(c, {}, [](const ConfirmationRequest&) { return true; });
    check(approved.status == AgentStatus::completed && tool->calls == 1, "Read-only permits approved read");
    check(c.profile.permissions.set_decision(OperationClass::read_file, PermissionDecision::deny), "Deny next run");
    (void)runtime.execute(c);
    check(tool->calls == 1 && fake->exposed.empty() && fake->responses[0].result.contains("error"),
          "Permissions and registry rebuilt for each run");
    c.profile.max_steps = static_cast<std::size_t>(std::numeric_limits<int>::max()) + 1;
    const auto starts = fake->starts;
    check(runtime.execute(c).error->code == "invalid_budget" && fake->starts == starts, "Oversized budget rejected before provider");
}

void cancellation_from_another_thread() {
    auto provider = std::make_unique<FakeProvider>();
    auto* fake = provider.get();
    std::latch entered{1}, cancelled{1};
    fake->on_start = [&] { entered.count_down(); cancelled.wait(); };
    fake->on_cancel = [&] { cancelled.count_down(); };
    AgentRuntime runtime{std::move(provider), "offline-placeholder", "host-selected"};
    const auto c = context();
    auto result = std::async(std::launch::async, [&] { return runtime.execute(c); });
    entered.wait();
    runtime.cancel_active_request();
    check(result.get().status == AgentStatus::cancelled, "Cross-thread cancellation unblocks provider");
    check(fake->cancellations == 1 && fake->history.empty(), "Cross-thread cleanup");
}
} // namespace

int main() {
    try {
        basic_and_isolation();
        permissions_and_tools();
        failures_and_cancellation();
        budgets_and_capabilities();
        approval_cancellation_and_policy_isolation();
        cancellation_from_another_thread();
        std::cout << "All AgentRuntime offline tests passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "AgentRuntime test failed: " << e.what() << '\n';
        return 1;
    }
}
