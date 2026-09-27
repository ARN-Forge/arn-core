#pragma once

#include "arn/core/agent/agent_profile.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace arn::core {

// Owns an in-memory catalog of independent AgentProfile values. The registry
// describes available agents; AgentRuntime executes one selected profile, and
// AgentSession owns the mutable state of one execution.
class AgentRegistry {
public:
    // Registers a profile by stable id. Empty ids and duplicate ids are
    // rejected without replacing an existing profile.
    [[nodiscard]] bool register_profile(AgentProfile profile);

    // Returns an independent copy so callers cannot mutate registry state.
    [[nodiscard]] std::optional<AgentProfile> find(std::string_view id) const;

    // Returns independent copies ordered by stable id.
    [[nodiscard]] std::vector<AgentProfile> profiles() const;

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

private:
    std::map<std::string, AgentProfile, std::less<>> profiles_;
};

namespace standard_agents {

inline constexpr std::string_view explorer_id = "explorer";
inline constexpr std::string_view planner_id = "planner";
inline constexpr std::string_view coder_id = "coder";
inline constexpr std::string_view reviewer_id = "reviewer";

[[nodiscard]] AgentProfile explorer();
[[nodiscard]] AgentProfile planner();
[[nodiscard]] AgentProfile coder();
[[nodiscard]] AgentProfile reviewer();

// Creates a fresh, independent registry containing exactly the four standard
// profiles. No mutable process-global registry is used.
[[nodiscard]] AgentRegistry make_registry();

} // namespace standard_agents

} // namespace arn::core
