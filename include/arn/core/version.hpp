/**
 * @file version.hpp
 * @brief ARN Core library version macros and constants.
 */

#pragma once

#include <string_view>

/// Major version number of the ARN Core library.
#define ARN_CORE_VERSION_MAJOR 0
/// Minor version number of the ARN Core library.
#define ARN_CORE_VERSION_MINOR 1
/// Patch version number of the ARN Core library.
#define ARN_CORE_VERSION_PATCH 0
/// Full version string of the ARN Core library.
#define ARN_CORE_VERSION_STRING "0.1.0"

namespace arn::core {

/// Major version integer constant.
inline constexpr int version_major = ARN_CORE_VERSION_MAJOR;
/// Minor version integer constant.
inline constexpr int version_minor = ARN_CORE_VERSION_MINOR;
/// Patch version integer constant.
inline constexpr int version_patch = ARN_CORE_VERSION_PATCH;

/// SemVer version string constant.
inline constexpr std::string_view version_string = ARN_CORE_VERSION_STRING;

/**
 * @brief Returns the ARN Core library version string.
 * @return SemVer string view (e.g. "0.1.0").
 */
[[nodiscard]] constexpr std::string_view version() noexcept {
    return version_string;
}

} // namespace arn::core
