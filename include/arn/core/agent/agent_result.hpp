/** @file agent_result.hpp
 * @brief Structured outcome without provider-specific errors or exceptions.
 */
#pragma once
#include "arn/core/agent/context_artifact.hpp"
#include <optional>
#include <string>
#include <vector>

namespace arn::core {
/// Outcomes of a future execution, including human intervention requirements.
enum class AgentStatus { completed, failed, cancelled, needs_input, needs_confirmation };

/** @brief Provider-independent diagnostic data. */
struct AgentError {
    /// Application-defined stable diagnostic identifier.
    std::string code;
    std::string message;
};

/** @brief Owned outcome and publishable artifacts; default does not claim success. */
struct AgentResult {
    AgentStatus status{AgentStatus::needs_input};
    std::string summary;
    std::vector<ContextArtifact> artifacts;
    std::optional<AgentError> error;
};
} // namespace arn::core
