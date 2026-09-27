#include "arn/core/agent/agent_registry.hpp"
#include "arn/core/agent/agent_runtime.hpp"

#include <atomic>
#include <memory>
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

class RegistryFakeProvider final : public IModelProvider {
public:
    std::vector<ToolDefinition> exposed;
    int starts{0};
    int continues{0};

    ProviderType type() const noexcept override { return ProviderType::custom; }
    std::string_view name() const noexcept override { return "Offline registry fake"; }
    std::string preferred_model(const std::vector<std::string>&) const override { return "unused"; }
    ModelCapabilities model_capabilities(std::string_view) const override { return {}; }
    ApiResult list_models(const std::string&, const std::atomic_bool*) override {
        throw std::runtime_error("Registry test must not discover models");
    }
    void trim_history(std::size_t) override {}
    void cancel_active_request() override {}
    void reset_session() override {}
    std::size_t session_entries() const noexcept override { return 0; }

    ModelTurn start_turn(const std::string&, const std::string&, const std::string&,
                         const std::string&, const ToolRegistry& tools,
                         const StreamCallbacks&, const std::atomic_bool*) override {
        ++starts;
        exposed = tools.definitions();
        return {.ok = true, .tool_calls = {{"read-1", "read_file", {{"path", "README.md"}}}}};
    }

    ModelTurn continue_turn(const std::string&, const std::string&, const std::string&,
                            const std::vector<ToolResponse>& responses, const ToolRegistry&,
                            const StreamCallbacks& callbacks, const std::atomic_bool*) override {
        ++continues;
        check(responses.size() == 1 && !responses[0].result.contains("error"), "Read tool result returned");
        if (callbacks.on_text) callbacks.on_text("Explorer inspected the project.");
        return {.ok = true, .text = "Explorer inspected the project."};
    }
};

class ReadTool final : public ITool {
public:
    int calls{0};
    ToolDefinition def{"read_file", "Read a project file", {{"type", "object"}}};
    const ToolDefinition& definition() const noexcept override { return def; }
    bool requires_confirmation() const noexcept override { return false; }
    ToolResult execute(const nlohmann::json&, const ToolContext&) override {
        ++calls;
        return ToolResult::success({{"content", "offline project contents"}});
    }
};

bool contains(std::string_view text, std::string_view value) {
    return text.find(value) != std::string_view::npos;
}

void check_read_only(const AgentProfile& profile) {
    check(profile.permissions.decision_for(OperationClass::read_file) == PermissionDecision::allow, "Read permitted");
    check(profile.permissions.decision_for(OperationClass::write_file) == PermissionDecision::deny, "Write denied");
    check(profile.permissions.decision_for(OperationClass::delete_file) == PermissionDecision::deny, "Delete denied");
    check(profile.permissions.decision_for(OperationClass::run_command) == PermissionDecision::deny, "Command denied");
    check(profile.permissions.decision_for(OperationClass::network) == PermissionDecision::deny, "Network denied");
    check(profile.permissions.decision_for(OperationClass::git_commit) == PermissionDecision::deny, "Commit denied");
    check(profile.permissions.decision_for(OperationClass::git_push) == PermissionDecision::deny, "Push denied");
    check(profile.permissions.decision_for(static_cast<OperationClass>(999)) == PermissionDecision::deny,
          "Unknown operation denied");
}

void standard_catalog() {
    AgentRegistry registry = standard_agents::make_registry();
    check(registry.size() == 4, "Exactly four standard profiles");
    const auto profiles = registry.profiles();
    check(profiles.size() == 4, "Four profiles enumerated");
    check(profiles[0].id == "coder" && profiles[1].id == "explorer"
              && profiles[2].id == "planner" && profiles[3].id == "reviewer",
          "Profiles have stable unique ids and deterministic order");

    for (const auto& profile : profiles) {
        check(!profile.display_name.empty() && !profile.instructions.empty(), "Profile text is non-empty");
        check(!contains(profile.instructions, "Gemini") && !contains(profile.instructions, "DeepSeek")
                  && !contains(profile.instructions, "OpenAI") && !contains(profile.instructions, "Anthropic"),
              "Instructions are provider agnostic");
    }

    const auto explorer = registry.find("explorer");
    const auto planner = registry.find("planner");
    const auto coder = registry.find("coder");
    const auto reviewer = registry.find("reviewer");
    check(explorer && explorer->display_name == "Explorer", "Explorer lookup");
    check(planner && planner->display_name == "Planner", "Planner lookup");
    check(coder && coder->display_name == "Coder", "Coder lookup");
    check(reviewer && reviewer->display_name == "Reviewer", "Reviewer lookup");
    check(!registry.find("unknown"), "Unknown lookup is empty");

    check_read_only(*explorer);
    check_read_only(*planner);
    check_read_only(*reviewer);
    check(coder->permissions.decision_for(OperationClass::read_file) == PermissionDecision::allow, "Coder reads");
    check(coder->permissions.decision_for(OperationClass::write_file) == PermissionDecision::ask_user,
          "Coder writes require approval");
    check(coder->permissions.decision_for(OperationClass::delete_file) == PermissionDecision::ask_user,
          "Coder deletes require approval");
    check(coder->permissions.decision_for(OperationClass::run_command) == PermissionDecision::deny,
          "Coder is not unrestricted");
    check(coder->permissions.decision_for(OperationClass::network) == PermissionDecision::deny,
          "Coder network denied");
    check(coder->permissions.decision_for(OperationClass::git_push) == PermissionDecision::deny,
          "Coder push denied");
}

void custom_profiles_duplicates_and_isolation() {
    AgentRegistry registry = standard_agents::make_registry();
    AgentProfile custom;
    custom.id = "custom";
    custom.display_name = "Custom";
    custom.instructions = "Answer from supplied context.";
    check(custom.permissions.set_decision(OperationClass::read_file, PermissionDecision::allow), "Set custom policy");
    check(registry.register_profile(custom), "Custom profile registered");
    check(registry.size() == 5, "Custom coexists with standard profiles");

    AgentProfile duplicate = custom;
    duplicate.display_name = "Replacement";
    check(!registry.register_profile(std::move(duplicate)), "Duplicate rejected deterministically");
    check(registry.find("custom")->display_name == "Custom", "Duplicate did not replace original");
    check(!registry.register_profile(AgentProfile{}), "Empty id rejected");

    AgentProfile detached = *registry.find("custom");
    detached.display_name = "Mutated Copy";
    check(detached.permissions.set_decision(OperationClass::write_file, PermissionDecision::allow), "Mutate copy");
    const auto stored = registry.find("custom");
    check(stored->display_name == "Custom", "Lookup mutation is isolated");
    check(stored->permissions.decision_for(OperationClass::write_file) == PermissionDecision::deny,
          "Stored permissions are isolated");

    AgentRegistry independent = standard_agents::make_registry();
    check(independent.size() == 4 && !independent.find("custom"), "Factory registries are independent");
}

void explorer_runtime_integration() {
    AgentRegistry registry = standard_agents::make_registry();
    auto explorer = registry.find("explorer");
    check(explorer.has_value(), "Explorer available for runtime");

    auto provider = std::make_unique<RegistryFakeProvider>();
    auto* fake = provider.get();
    auto tool = std::make_shared<ReadTool>();
    AgentRuntime runtime{std::move(provider), "offline-placeholder", "host-selected",
                         {{tool, {OperationClass::read_file}}}};
    AgentContext context;
    context.profile = *explorer;
    context.task.id = "registry-integration";
    context.task.objective = "Inspect the project";
    context.task.working_directory = "virtual-workspace";

    const AgentResult result = runtime.execute(context);
    check(result.status == AgentStatus::completed, "Standard Explorer completes through runtime");
    check(result.summary == "Explorer inspected the project.", "Runtime returns fake response");
    check(fake->starts == 1 && fake->continues == 1, "Runtime completed one tool loop");
    check(fake->exposed.size() == 1 && fake->exposed[0].name == "read_file", "Only permitted tool exposed");
    check(tool->calls == 1, "Read tool executed once");
}

} // namespace

int main() {
    standard_catalog();
    custom_profiles_duplicates_and_isolation();
    explorer_runtime_integration();
    return 0;
}
