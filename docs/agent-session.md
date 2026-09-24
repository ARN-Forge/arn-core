# AgentSession Guide

The `arn::core::AgentSession` class is the central coordinator of ARN Core. It connects language model backends (`IModelProvider`), tool collections (`ToolRegistry`), confirmation gates (`ConfirmationFn`), and client streaming callbacks into a coherent multi-turn conversational loop.

**Header**: [`include/arn/core/agent/agent_session.hpp`](../include/arn/core/agent/agent_session.hpp)

---

## 1. Configuration: `AgentConfig`

`AgentConfig` sets parameters governing turn behavior and session limits:

```cpp
struct AgentConfig {
    /// System prompt or persona guiding model behavior across turns.
    std::string system_instruction;

    /// Maximum consecutive rounds of tool calls allowed within a single user turn.
    /// Default: 12.
    int max_tool_rounds{12};

    /// Maximum number of messages kept in provider conversation history before
    /// older turns are trimmed. Default: 40.
    std::size_t max_history_entries{40};
};
```

### Configuration Best Practices
- **`max_tool_rounds`**: Guards against recursive or runaway tool loops where a model repeatedly calls tools without reaching a conclusion. If this threshold is exceeded, `prompt()` returns `ApiResult{ .ok = false, .message = "Stopped after too many tool calls." }`.
- **`max_history_entries`**: Constrains memory and token consumption. Before starting a turn, `AgentSession` automatically commands the provider to prune older conversation turns while keeping the initial system instruction and recent context intact.

---

## 2. Instantiation & Lifecycles

`AgentSession` is movable but non-copyable:

```cpp
#include <arn/core/agent/agent_session.hpp>

// Default configuration
arn::core::AgentSession session;

// Or customized configuration
arn::core::AgentConfig config{
    .system_instruction = "You are a code refactoring expert.",
    .max_tool_rounds = 8,
    .max_history_entries = 30
};
arn::core::AgentSession session(std::move(config));
```

The session configuration can also be updated after instantiation:

```cpp
session.set_system_instruction("You are now in debug mode.");
std::cout << "Instruction: " << session.system_instruction() << '\n';
```

---

## 3. Provider Configuration

`AgentSession` provides flexible ways to configure its underlying LLM provider.

### Option A: Automatic Model Discovery (Recommended)

Pass a `ProviderType` and API key. ARN Core automatically queries the endpoint's models catalog, populates `available_models()`, and selects the provider's `preferred_model()`:

```cpp
auto result = session.configure_provider(arn::core::ProviderType::gemini, api_key);
if (!result.ok) {
    std::cerr << "Failed to configure provider: " << result.message << '\n';
    return 1;
}

std::cout << "Preferred model: " << session.preferred_model() << '\n';
std::cout << "Active model: " << session.active_model() << '\n';

// Inspect all discovered models
for (const auto& model : session.available_models()) {
    std::cout << " - " << model << '\n';
}

// Optionally switch to a specific model
session.select_model("gemini-1.5-pro");
```

### Option B: Custom / Configured Provider Instance

Pass a `std::shared_ptr<IModelProvider>` when you need custom provider configurations (such as custom endpoints, OpenRouter referral headers, or custom proxies):

```cpp
#include <arn/core/provider/openrouter_provider.hpp>

arn::core::OpenRouterConfig router_cfg{
    .endpoint = "https://openrouter.ai",
    .http_referer = "https://my-app.example.com",
    .app_title = "My Desktop Agent"
};
auto provider = std::make_shared<arn::core::OpenRouterProvider>(router_cfg);

session.configure_provider(provider, api_key);
session.select_model("anthropic/claude-3.5-sonnet");
```

### Option C: Non-Owning External Pointer

If the host application manages the provider's lifetime externally:

```cpp
arn::core::GeminiProvider custom_gemini;
session.set_provider(&custom_gemini, api_key);
session.select_model("gemini-2.0-flash");
```

> [!IMPORTANT]
> When using `set_provider()`, model discovery is not executed automatically. You must explicitly call `select_model()` with your desired model identifier before calling `prompt()`.

---

## 4. Tool Registry & Confirmation Attachment

Attach your tools and confirmation handlers before executing prompts:

```cpp
auto tools = std::make_shared<arn::core::ToolRegistry>();
tools->register_tool(std::make_shared<MyTool>());
session.set_tools(tools);

// Optional: attach human confirmation handler
session.set_confirmation_handler([](const arn::core::ConfirmationRequest& req) {
    std::cout << "\n[CONFIRMATION] Tool '" << req.name << "' requested execution:\n"
              << "  Summary: " << req.summary << "\n"
              << "Approve? (y/n): " << std::flush;
    char answer;
    std::cin >> answer;
    return answer == 'y' || answer == 'Y';
});
```

---

## 5. Multi-Turn Prompting

The `prompt()` method executes the complete turn loop: sending the user query, streaming responses, resolving tool calls, and returning the final text.

### Signature & Overloads

```cpp
// 1. Full overload with callbacks and cancellation
ApiResult prompt(std::string_view text,
                 const StreamCallbacks& callbacks = {},
                 const std::atomic_bool* cancel = nullptr);

// 2. Convenience overload with only text streaming callback
ApiResult prompt(std::string_view text,
                 const TextStreamCallback& on_text);

// 3. Convenience overload with only cancellation flag
ApiResult prompt(std::string_view text,
                 const std::atomic_bool* cancel);

// 4. Convenience overload with text streaming, cancellation, and progress callback
ApiResult prompt(std::string_view text,
                 const TextStreamCallback& on_text,
                 const std::atomic_bool* cancel,
                 const ProgressCallback& on_progress);
```

### Handling Multi-Turn Conversations

`AgentSession` delegates conversation memory to the active provider. Subsequent calls to `prompt()` preserve conversation history:

```cpp
// Turn 1
session.prompt("My name is Alice.", [](std::string_view delta) {
    std::cout << delta << std::flush;
});
std::cout << "\n";

// Turn 2
session.prompt("What is my name?", [](std::string_view delta) {
    std::cout << delta << std::flush;
});
// Agent outputs: "Your name is Alice."
```

### Inspecting & Resetting Session History

```cpp
std::size_t history_count = session.session_entries();
std::cout << "Recorded history turns: " << history_count << '\n';

// Reset session to clear message history without losing provider configuration
session.reset_session();
std::cout << "History turns after reset: " << session.session_entries() << '\n'; // 0
```

---

## 6. Model Capabilities Inspection

Different models support different feature subsets (e.g. some models lack function calling or streaming). You can query capabilities at runtime:

```cpp
arn::core::ModelCapabilities caps = session.model_capabilities();
if (!caps.supports_tools) {
    std::cout << "Warning: The selected model does not support tool calling.\n";
}
if (!caps.supports_streaming) {
    std::cout << "Note: Responses will be returned as complete blocks.\n";
}
```

---

## 7. Cancellation

Prompt execution can be aborted at any time using either cooperative cancellation tokens or the thread-safe `cancel_active_request()` method:

### Method 1: Atomic Cancellation Flag

```cpp
std::atomic_bool cancel_flag{false};

// Spawn prompt in a worker thread
std::thread worker([&]() {
    auto res = session.prompt("Write a 5,000 word essay", callbacks, &cancel_flag);
    if (res.cancelled) {
        std::cout << "\nTask was cancelled.\n";
    }
});

// Cancel from another thread
std::this_thread::sleep_for(std::chrono::milliseconds(500));
cancel_flag.store(true);
worker.join();
```

### Method 2: `cancel_active_request()`

Useful for UI Cancel buttons or Ctrl+C signal handlers:

```cpp
// Thread-safe: immediately aborts in-flight network transfers and breaks the turn loop
session.cancel_active_request();
```
