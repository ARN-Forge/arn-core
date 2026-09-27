/** @file agent_profile.hpp
 * @brief Data-driven agent configuration, not execution.
 */
#pragma once
#include "arn/core/agent/agent_permissions.hpp"
#include "arn/core/agent/model_policy.hpp"
#include <cstddef>
#include <string>
#include <vector>

namespace arn::core {
/** @brief Owned configuration; never owns an AgentSession or constructs providers. */
struct AgentProfile {
    /// Stable machine-readable identifier, independent of the human display name.
    std::string id;
    std::string display_name;
    std::string description;
    /// Behavioral guidance only; cannot override permissions.
    std::string instructions;
    /// Tool identifiers, not registrations or permission grants. Empty means none.
    std::vector<std::string> allowed_tools;
    /// Skill identifiers only; no discovery or loading is performed.
    std::vector<std::string> default_skills;
    AgentPermissions permissions;
    ModelPolicy model_policy;
    /// Future execution budget; callers must keep this non-zero.
    std::size_t max_steps{12};
};
} // namespace arn::core
