/** @file agent_context.hpp
 * @brief Isolated state for one future agent execution.
 */
#pragma once
#include "arn/core/agent/agent_profile.hpp"
#include "arn/core/agent/agent_task.hpp"
#include <vector>

namespace arn::core {
/** @brief Owns independent profile/task/artifact values for one execution.
 * Copies do not share mutable state. No session or conversation history is stored
 * or implicitly transferred. Cross-agent information is passed explicitly through
 * ContextArtifact values. This type performs no work at construction or destruction.
 */
struct AgentContext {
    AgentProfile profile;
    AgentTask task;
    /// Local artifacts; task.inputs remains the explicit initial input collection.
    std::vector<ContextArtifact> artifacts;
};
} // namespace arn::core
