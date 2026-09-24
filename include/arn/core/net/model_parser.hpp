/**
 * @file model_parser.hpp
 * @brief Utilities for parsing model catalog responses from LLM endpoints.
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace arn::core::net {

/**
 * @brief Parses a list of available model identifiers from raw JSON HTTP response bodies.
 * @param body JSON string received from the provider's models endpoint.
 * @param gemini True if parsing Gemini models format; false for standard OpenAI-style format.
 * @return Vector of model identifier strings.
 */
[[nodiscard]] std::vector<std::string> parse_model_list(std::string_view body, bool gemini);

} // namespace arn::core::net
