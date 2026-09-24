/**
 * @file tool_registry.hpp
 * @brief Registry for storing, querying, and dispatching agent tools.
 */

#pragma once

#include "arn/core/tool/tool.hpp"

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace arn::core {

/**
 * @brief Thread-compatible collection of registered tools.
 *
 * Provides registration, lookup, schema aggregation for LLM prompts,
 * and dispatching of tool execution requests.
 */
class ToolRegistry {
public:
    ToolRegistry() = default;

    /**
     * @brief Registers a new tool in the registry.
     * @param tool Shared pointer to the tool instance.
     * @return True if registered; false if tool is null or a tool with the same name already exists.
     */
    bool register_tool(std::shared_ptr<ITool> tool);

    /**
     * @brief Removes a tool from the registry by name.
     * @param name Name of the tool to unregister.
     * @return True if the tool was found and removed; false otherwise.
     */
    bool unregister_tool(std::string_view name);

    /**
     * @brief Looks up a tool by name.
     * @param name Name of the tool.
     * @return Shared pointer to the tool, or nullptr if not found.
     */
    [[nodiscard]] std::shared_ptr<ITool> find_tool(std::string_view name) const;

    /**
     * @brief Checks if a tool with the specified name is registered.
     * @param name Name of the tool.
     * @return True if registered, false otherwise.
     */
    [[nodiscard]] bool has_tool(std::string_view name) const;

    /**
     * @brief Retrieves definitions of all registered tools.
     * @return Vector of ToolDefinition structs.
     */
    [[nodiscard]] std::vector<ToolDefinition> definitions() const;

    /**
     * @brief Generates a JSON array of tool definitions formatted for model function declarations.
     * @return nlohmann::json array of tool objects with name, description, and parameters.
     */
    [[nodiscard]] nlohmann::json definitions_json() const;

    /**
     * @brief Executes a tool by name.
     * @param name Name of the tool to execute.
     * @param arguments JSON arguments to pass to the tool.
     * @param context Execution context with cancellation token and confirmation handler.
     * @return ToolResult from the tool, or ToolResult::failure if tool is unknown.
     */
    [[nodiscard]] ToolResult execute(const std::string& name, const nlohmann::json& arguments,
                                     const ToolContext& context = {}) const;

    /// Returns the number of registered tools.
    [[nodiscard]] std::size_t size() const noexcept;
    /// Checks whether the registry is empty.
    [[nodiscard]] bool empty() const noexcept;
    /// Clears all registered tools.
    void clear();

private:
    std::map<std::string, std::shared_ptr<ITool>, std::less<>> tools_;
};

} // namespace arn::core
