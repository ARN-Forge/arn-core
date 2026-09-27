/** @file model_policy.hpp
 * @brief Provider-independent model preference metadata, without routing.
 */
#pragma once
#include <optional>

namespace arn::core {
/// Preferences, not concrete model IDs or guarantees of model availability.
enum class ModelCapability { default_model, fast, reasoning, coding, cheap, long_context };

/** @brief Capability preferences only; does not select or construct a provider. */
struct ModelPolicy {
    ModelCapability preferred{ModelCapability::default_model};
    std::optional<ModelCapability> fallback;
};
} // namespace arn::core
