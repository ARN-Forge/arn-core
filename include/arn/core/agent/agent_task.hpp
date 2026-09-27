/** @file agent_task.hpp
 * @brief Explicit provider-independent work description.
 */
#pragma once
#include "arn/core/agent/context_artifact.hpp"
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace arn::core {
/** @brief Small declarative constraints; no execution or authorization is performed. */
struct TaskConstraints {
    /// Absent uses the profile budget; zero is invalid.
    std::optional<std::size_t> max_steps;
    /// Restrictive intent only; false never grants write permission.
    bool read_only{false};
    std::string notes;
};

/** @brief Value-owned work item and explicitly supplied input artifacts. */
struct AgentTask {
    std::string id;
    std::string objective;
    /// Caller-supplied path; this type never inspects or resolves the filesystem.
    std::filesystem::path working_directory;
    std::vector<ContextArtifact> inputs;
    TaskConstraints constraints;

    /// Structural validation only: required text/path and a positive optional budget.
    /// Does not verify path existence, access, or containment.
    [[nodiscard]] bool valid() const {
        return id.find_first_not_of(" \t\r\n") != std::string::npos
            && objective.find_first_not_of(" \t\r\n") != std::string::npos
            && !working_directory.empty()
            && (!constraints.max_steps || *constraints.max_steps > 0);
    }
};
} // namespace arn::core
