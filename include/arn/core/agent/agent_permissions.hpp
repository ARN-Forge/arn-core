/** @file agent_permissions.hpp
 * @brief Conservative policy metadata for a future runtime.
 */
#pragma once
#include <map>

namespace arn::core {
/// Policy outcome; ask_user is not an authorization until approval is obtained.
enum class PermissionDecision { allow, deny, ask_user };
/// Categories independent of provider-specific tool definitions.
enum class OperationClass { read_file, write_file, delete_file, run_command, network, git_commit, git_push };

/** @brief Value-owned policy configured by the trusted host, never by model instructions.
 * Unconfigured and unknown operations are denied. This type does not enforce policy
 * or change existing tool confirmations; a future runtime must enforce it before
 * executing tools. Prompt text and task intent cannot grant permissions.
 */
class AgentPermissions {
public:
    /// Returns deny for every operation without an explicit trusted-host decision.
    [[nodiscard]] PermissionDecision decision_for(OperationClass operation) const {
        switch (operation) {
        case OperationClass::read_file: case OperationClass::write_file:
        case OperationClass::delete_file: case OperationClass::run_command:
        case OperationClass::network: case OperationClass::git_commit:
        case OperationClass::git_push:
            if (const auto it = decisions_.find(operation); it != decisions_.end())
                return it->second;
            break;
        }
        return PermissionDecision::deny;
    }

    /// Configures a known operation; returns false for invalid enum values.
    [[nodiscard]] bool set_decision(OperationClass operation, PermissionDecision decision) {
        switch (decision) {
        case PermissionDecision::allow: case PermissionDecision::deny: case PermissionDecision::ask_user:
            break;
        default: return false;
        }
        switch (operation) {
        case OperationClass::read_file: case OperationClass::write_file:
        case OperationClass::delete_file: case OperationClass::run_command:
        case OperationClass::network: case OperationClass::git_commit:
        case OperationClass::git_push:
            decisions_[operation] = decision;
            return true;
        }
        return false;
    }
private:
    std::map<OperationClass, PermissionDecision> decisions_;
};
} // namespace arn::core
