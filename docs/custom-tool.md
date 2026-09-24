# Writing a Custom Tool

This tutorial guides you through creating, testing, and registering custom tools with **ARN Core** (`arn::core`).

---

## 1. Overview

Tools allow models to query dynamic data or trigger operations in the real world. In ARN Core, tools:
- Implement the `arn::core::ITool` interface.
- Expose metadata and parameters through a standard JSON Schema (`ToolDefinition`).
- Receive an execution context (`ToolContext`) with cancellation tokens and confirmation callbacks.
- Return structured JSON payloads or error descriptions (`ToolResult`).

---

## 2. Step-by-Step Implementation: `FileReadTool`

We will build a safe, read-only file reading tool with line-range constraints and cancellation checks.

### Step 1: Subclass `ITool`

Create a new class inheriting from `arn::core::ITool`:

```cpp
#include <arn/core/tool/tool.hpp>
#include <fstream>
#include <string>
#include <vector>

class FileReadTool final : public arn::core::ITool {
public:
    const arn::core::ToolDefinition& definition() const noexcept override;
    bool requires_confirmation() const noexcept override;
    arn::core::ToolResult execute(const nlohmann::json& arguments,
                                  const arn::core::ToolContext& context) override;
};
```

---

### Step 2: Define Metadata and Parameter Schema

The `definition()` method provides the LLM with the tool's name, description, and expected argument types:

```cpp
const arn::core::ToolDefinition& FileReadTool::definition() const noexcept {
    static const arn::core::ToolDefinition def{
        .name = "read_file",
        .description = "Reads text lines from a local file within a specified range.",
        .parameter_schema = {
            {"type", "object"},
            {"properties", {
                {"path", {
                    {"type", "string"},
                    {"description", "Absolute or relative path to the file"}
                }},
                {"start_line", {
                    {"type", "integer"},
                    {"description", "1-based starting line number (default: 1)"},
                    {"default", 1}
                }},
                {"max_lines", {
                    {"type", "integer"},
                    {"description", "Maximum lines to read (1-500, default: 100)"},
                    {"default", 100}
                }}
            }},
            {"required", {"path"}}
        }
    };
    return def;
}
```

---

### Step 3: Configure Confirmation Requirements

If a tool only reads information, it usually does not require confirmation:

```cpp
bool FileReadTool::requires_confirmation() const noexcept {
    return false; // Safe read-only tool
}
```

*(Note: For mutating tools such as file writers or command runners, return `true` or dynamically invoke `context.confirm()`.)*

---

### Step 4: Implement `execute()`

The execution function must:
1. Parse and validate arguments safely.
2. Check `context.cancel_requested` for cooperative aborts.
3. Catch all exceptions to prevent crashes.
4. Return `ToolResult::success` or `ToolResult::failure`.

```cpp
arn::core::ToolResult FileReadTool::execute(const nlohmann::json& arguments,
                                            const arn::core::ToolContext& context) {
    try {
        // 1. Validate mandatory fields
        if (!arguments.contains("path") || !arguments["path"].is_string()) {
            return arn::core::ToolResult::failure("Missing or invalid string argument 'path'.");
        }

        const std::string path = arguments["path"].get<std::string>();
        const int start_line = arguments.value("start_line", 1);
        const int max_lines = std::min(arguments.value("max_lines", 100), 500);

        // 2. Open file
        std::ifstream file(path);
        if (!file.is_open()) {
            return arn::core::ToolResult::failure("Unable to open file: " + path);
        }

        std::vector<std::string> lines;
        std::string line;
        int current_line = 1;

        while (std::getline(file, line)) {
            // 3. Check for cancellation periodically
            if (context.cancel_requested && context.cancel_requested->load()) {
                return arn::core::ToolResult::failure("File read cancelled by user.");
            }

            if (current_line >= start_line && static_cast<int>(lines.size()) < max_lines) {
                lines.push_back(line);
            }

            if (static_cast<int>(lines.size()) >= max_lines) {
                break;
            }
            ++current_line;
        }

        // 4. Return structured JSON payload
        return arn::core::ToolResult::success({
            {"path", path},
            {"start_line", start_line},
            {"line_count", lines.size()},
            {"lines", lines}
        });

    } catch (const std::exception& ex) {
        // Catch exceptions and return clean failure messages
        return arn::core::ToolResult::failure(std::string("Internal error: ") + ex.what());
    }
}
```

---

## 3. Unit Testing Without an LLM

You can test custom tools directly using `ToolRegistry::execute()` without making API calls:

```cpp
#include <cassert>
#include <iostream>
#include <arn/core/tool/tool_registry.hpp>

int main() {
    arn::core::ToolRegistry registry;
    registry.register_tool(std::make_shared<FileReadTool>());

    // Test missing path parameter
    auto res1 = registry.execute("read_file", nlohmann::json::object());
    assert(!res1.ok);
    std::cout << "Missing argument error: " << res1.error_message << "\n";

    // Test successful execution
    auto res2 = registry.execute("read_file", {{"path", "CMakeLists.txt"}, {"max_lines", 5}});
    assert(res2.ok);
    std::cout << "Read " << res2.result["line_count"] << " lines successfully.\n";

    return 0;
}
```

---

## 4. Registering with `AgentSession`

Once tested, register your tool with an `AgentSession`:

```cpp
#include <arn/core/agent/agent_session.hpp>

auto tools = std::make_shared<arn::core::ToolRegistry>();
tools->register_tool(std::make_shared<FileReadTool>());

arn::core::AgentSession session;
session.set_tools(tools);
session.configure_provider(arn::core::ProviderType::gemini, api_key);

// The agent can now autonomously invoke "read_file" when needed!
session.prompt("What is the project version defined in CMakeLists.txt?",
               [](std::string_view delta) { std::cout << delta << std::flush; });
```
