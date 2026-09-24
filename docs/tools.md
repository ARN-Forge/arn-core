# Tool System

ARN Core provides a clean, schema-driven tool calling subsystem. Tools allow language models to inspect external state, run calculations, and interact with outside APIs and local subsystems.

**Headers**:
- Tool interface & structures: [`include/arn/core/tool/tool.hpp`](../include/arn/core/tool/tool.hpp)
- Tool registry: [`include/arn/core/tool/tool_registry.hpp`](../include/arn/core/tool/tool_registry.hpp)

---

## 1. The `ITool` Interface

Every tool in ARN Core implements the abstract `arn::core::ITool` base class:

```cpp
#include <arn/core/tool/tool.hpp>

class ITool {
public:
    virtual ~ITool() = default;

    /// Returns the static metadata and JSON Schema describing this tool.
    [[nodiscard]] virtual const ToolDefinition& definition() const noexcept = 0;

    /// Indicates whether this tool requires human-in-the-loop approval before running.
    /// Default: false.
    [[nodiscard]] virtual bool requires_confirmation() const noexcept { return false; }

    /// Executes the tool with the JSON arguments provided by the model.
    [[nodiscard]] virtual ToolResult execute(const nlohmann::json& arguments,
                                             const ToolContext& context) = 0;
};
```

---

## 2. Defining Tools: `ToolDefinition` & JSON Schema

A `ToolDefinition` informs language models what the tool does and what parameters it accepts:

```cpp
struct ToolDefinition {
    /// Unique invocation identifier (e.g. "fetch_weather", "search_docs").
    std::string name;

    /// Clear, concise explanation of the tool's purpose and usage tips.
    std::string description;

    /// JSON Schema object defining parameter types, properties, and constraints.
    nlohmann::json parameter_schema;
};
```

### JSON Schema Conventions

Language models rely on standard JSON Schema objects to structure their function call arguments:

```cpp
static const arn::core::ToolDefinition def{
    .name = "search_records",
    .description = "Searches the customer database for records matching the query.",
    .parameter_schema = {
        {"type", "object"},
        {"properties", {
            {"query", {
                {"type", "string"},
                {"description", "Search query or customer name"}
            }},
            {"limit", {
                {"type", "integer"},
                {"description", "Maximum records to return (1-50)"},
                {"default", 10}
            }},
            {"include_inactive", {
                {"type", "boolean"},
                {"description", "Include closed or inactive accounts"}
            }}
        }},
        {"required", {"query"}}
    }
};
```

#### Best Practices for Schema Design
- Always specify `"type": "object"` at the top level.
- Provide clear `"description"` strings for every property; LLMs read these descriptions to decide when and how to fill arguments.
- Mark mandatory fields in the `"required"` array.
- Avoid unnecessarily complex or deeply nested schemas where flat schemas suffice.

---

## 3. Tool Execution & `ToolResult`

The `execute()` method receives:
1. `arguments`: Parsed `nlohmann::json` containing the arguments emitted by the model.
2. `context`: `arn::core::ToolContext` containing cooperative cancellation tokens and an optional confirmation callback.

### `ToolResult` Structure

```cpp
struct ToolResult {
    /// True if the tool completed successfully; false on error.
    bool ok{false};

    /// Structured result payload returned to the model as context.
    nlohmann::json result;

    /// Error description if execution failed.
    std::string error_message;

    // Helper constructors
    static ToolResult success(nlohmann::json output);
    static ToolResult failure(std::string message);
};
```

### Error Containment

> [!IMPORTANT]
> The `execute()` method must not throw unhandled exceptions. Catch any internal exceptions and return a clean failure via `ToolResult::failure(e.what())`. This allows the LLM to learn why the tool call failed and either retry with corrected arguments or apologize to the user.

```cpp
arn::core::ToolResult execute(const nlohmann::json& args,
                              const arn::core::ToolContext& context) override {
    try {
        if (!args.contains("query")) {
            return arn::core::ToolResult::failure("Missing required 'query' argument.");
        }
        
        // Execute business logic...
        nlohmann::json data = do_search(args["query"].get<std::string>());
        return arn::core::ToolResult::success(data);
    } catch (const std::exception& ex) {
        return arn::core::ToolResult::failure(ex.what());
    }
}
```

---

## 4. Execution Context: `ToolContext`

```cpp
struct ToolContext {
    /// Pointer to atomic cancellation flag, or nullptr if not monitored.
    const std::atomic_bool* cancel_requested{nullptr};

    /// Confirmation callback function for requesting approval.
    ConfirmationFn confirm;
};
```

### Checking for Cancellation
For long-running tools (e.g. database scans, network fetches, heavy computation), periodically check `cancel_requested`:

```cpp
for (const auto& item : items) {
    if (context.cancel_requested && context.cancel_requested->load()) {
        return arn::core::ToolResult::failure("Operation cancelled by user.");
    }
    process_item(item);
}
```

### Requesting Dynamic Confirmation
If an operation is only dangerous under certain conditions (e.g. deleting more than 10 records), the tool can dynamically invoke `context.confirm`:

```cpp
    arn::core::ConfirmationRequest req(
        "bulk_delete",
        args,
        "Delete " + std::to_string(record_count) + " records?",
        /*changes_state=*/true
    );
    if (!context.confirm(req)) {
        return arn::core::ToolResult::failure("Bulk delete rejected by user.");
    }
}
```

---

## 5. Managing Tools: `ToolRegistry`

The `arn::core::ToolRegistry` manages the collection of tools exposed to models.

```cpp
#include <arn/core/tool/tool_registry.hpp>

auto registry = std::make_shared<arn::core::ToolRegistry>();

// 1. Register tools
registry->register_tool(std::make_shared<SearchTool>());
registry->register_tool(std::make_shared<CalculatorTool>());

// 2. Query registry
if (registry->has_tool("search_records")) {
    auto tool = registry->find_tool("search_records");
}

std::size_t count = registry->size();
bool is_empty = registry->empty();

// 3. Export definitions for LLMs
nlohmann::json definitions_json = registry->definitions_json();

// 4. Dispatch tool execution directly
arn::core::ToolResult res = registry->execute("calculator", {{"a", 10}, {"b", 5}});

// 5. Unregister or clear
registry->unregister_tool("calculator");
registry->clear();
```

### Registration Rules
- Tool names must be unique within a registry. Registering a second tool with an existing name returns `false` and does not replace the existing tool.
- Registering a `nullptr` returns `false`.

### Thread-Safety & Immutability
`ToolRegistry` uses a `std::map<std::string, std::shared_ptr<ITool>, std::less<>>` for lookup. When attached to an `AgentSession`, pass it as a `std::shared_ptr<const ToolRegistry>` to ensure concurrent prompt execution can safely query definitions and dispatch tools.
