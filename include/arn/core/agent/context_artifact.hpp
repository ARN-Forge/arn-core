/** @file context_artifact.hpp
 * @brief Explicit information boundary between independent agent executions.
 */
#pragma once
#include <string>

namespace arn::core {
/// Semantic purpose of an artifact; generic supports application-defined content.
enum class ArtifactType {
    user_request, exploration_report, implementation_plan, change_summary,
    test_result, error_report, review_report, generic
};

/** @brief Owned textual output exchanged instead of private conversation histories. */
struct ContextArtifact {
    ArtifactType type{ArtifactType::generic};
    /// Stable producer identifier supplied by the caller.
    std::string producer;
    /// Arbitrary text, including application-defined formats.
    std::string content;
};
} // namespace arn::core
