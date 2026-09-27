#include "arn/core/agent/agent_registry.hpp"

#include <utility>

namespace arn::core {

namespace {

constexpr std::string_view kListFilesTool = "list_files";
constexpr std::string_view kReadFileTool = "read_file";
constexpr std::string_view kSearchFilesTool = "search_files";
constexpr std::string_view kWriteFileTool = "write_file";
constexpr std::string_view kDeleteFileTool = "delete_file";

AgentPermissions read_only_permissions() {
    AgentPermissions permissions;
    (void)permissions.set_decision(OperationClass::read_file, PermissionDecision::allow);
    return permissions;
}

std::vector<std::string> read_only_tools() {
    return {
        std::string(kListFilesTool),
        std::string(kReadFileTool),
        std::string(kSearchFilesTool),
    };
}

AgentProfile make_read_only_profile(std::string id,
                                    std::string display_name,
                                    std::string description,
                                    std::string instructions,
                                    ModelCapability preferred,
                                    ModelCapability fallback) {
    AgentProfile profile;
    profile.id = std::move(id);
    profile.display_name = std::move(display_name);
    profile.description = std::move(description);
    profile.instructions = std::move(instructions);
    profile.allowed_tools = read_only_tools();
    profile.permissions = read_only_permissions();
    profile.model_policy.preferred = preferred;
    profile.model_policy.fallback = fallback;
    return profile;
}

} // namespace

bool AgentRegistry::register_profile(AgentProfile profile) {
    if (profile.id.empty()) {
        return false;
    }

    const std::string id = profile.id;
    return profiles_.emplace(id, std::move(profile)).second;
}

std::optional<AgentProfile> AgentRegistry::find(std::string_view id) const {
    const auto it = profiles_.find(id);
    if (it == profiles_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<AgentProfile> AgentRegistry::profiles() const {
    std::vector<AgentProfile> result;
    result.reserve(profiles_.size());
    for (const auto& entry : profiles_) {
        result.push_back(entry.second);
    }
    return result;
}

std::size_t AgentRegistry::size() const noexcept {
    return profiles_.size();
}

bool AgentRegistry::empty() const noexcept {
    return profiles_.empty();
}

namespace standard_agents {

AgentProfile explorer() {
    return make_read_only_profile(
        std::string(explorer_id),
        "Explorer",
        "Inspects a project and reports relevant facts without changing it.",
        "Explore the project, locate relevant files and symbols, and report evidence. Do not modify files or run mutating operations.",
        ModelCapability::fast,
        ModelCapability::long_context);
}

AgentProfile planner() {
    return make_read_only_profile(
        std::string(planner_id),
        "Planner",
        "Produces implementation plans from project evidence without changing files.",
        "Inspect the project as needed, identify constraints and dependencies, and produce a concrete implementation plan. Do not modify files.",
        ModelCapability::reasoning,
        ModelCapability::default_model);
}

AgentProfile coder() {
    AgentProfile profile;
    profile.id = std::string(coder_id);
    profile.display_name = "Coder";
    profile.description = "Implements focused code changes with explicit approval for file mutations.";
    profile.instructions =
        "Inspect the relevant code, implement the requested change, and verify it. Request approval before writing or deleting files.";
    profile.allowed_tools = read_only_tools();
    profile.allowed_tools.emplace_back(kWriteFileTool);
    profile.allowed_tools.emplace_back(kDeleteFileTool);
    (void)profile.permissions.set_decision(OperationClass::read_file, PermissionDecision::allow);
    (void)profile.permissions.set_decision(OperationClass::write_file, PermissionDecision::ask_user);
    (void)profile.permissions.set_decision(OperationClass::delete_file, PermissionDecision::ask_user);
    profile.model_policy.preferred = ModelCapability::coding;
    profile.model_policy.fallback = ModelCapability::reasoning;
    return profile;
}

AgentProfile reviewer() {
    return make_read_only_profile(
        std::string(reviewer_id),
        "Reviewer",
        "Reviews project changes for correctness, safety and regressions without editing them.",
        "Inspect the relevant code and changes, prioritize concrete defects, and explain each finding with evidence. Do not modify files.",
        ModelCapability::reasoning,
        ModelCapability::coding);
}

AgentRegistry make_registry() {
    AgentRegistry registry;
    (void)registry.register_profile(explorer());
    (void)registry.register_profile(planner());
    (void)registry.register_profile(coder());
    (void)registry.register_profile(reviewer());
    return registry;
}

} // namespace standard_agents

} // namespace arn::core
