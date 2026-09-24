# Model Providers

ARN Core decouples agent orchestration from language model protocols via the `arn::core::IModelProvider` interface.

**Headers**:
- Common types & interface: [`include/arn/core/provider/model_provider.hpp`](../include/arn/core/provider/model_provider.hpp)
- Google Gemini: [`include/arn/core/provider/gemini_provider.hpp`](../include/arn/core/provider/gemini_provider.hpp)
- DeepSeek: [`include/arn/core/provider/deepseek_provider.hpp`](../include/arn/core/provider/deepseek_provider.hpp)
- OpenRouter: [`include/arn/core/provider/openrouter_provider.hpp`](../include/arn/core/provider/openrouter_provider.hpp)

---

## 1. Provider Architecture & Interface

All backends implement the `arn::core::IModelProvider` abstract base class:

```cpp
class IModelProvider {
public:
    virtual ~IModelProvider() = default;

    [[nodiscard]] virtual ProviderType type() const noexcept = 0;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    [[nodiscard]] virtual std::string
    preferred_model(const std::vector<std::string>& models) const = 0;

    [[nodiscard]] virtual ModelCapabilities model_capabilities(std::string_view model) const {
        return ModelCapabilities{};
    }

    [[nodiscard]] virtual ApiResult
    list_models(const std::string& api_key,
                const std::atomic_bool* cancel_requested = nullptr) = 0;

    [[nodiscard]] virtual ModelTurn
    start_turn(const std::string& api_key, const std::string& model,
               const std::string& system_instruction, const std::string& user_prompt,
               const ToolRegistry& tools,
               const StreamCallbacks& callbacks = {},
               const std::atomic_bool* cancel_requested = nullptr) = 0;

    [[nodiscard]] virtual ModelTurn
    continue_turn(const std::string& api_key, const std::string& model,
                  const std::string& system_instruction,
                  const std::vector<ToolResponse>& tool_responses,
                  const ToolRegistry& tools,
                  const StreamCallbacks& callbacks = {},
                  const std::atomic_bool* cancel_requested = nullptr) = 0;

    virtual void trim_history(std::size_t max_entries) = 0;
    virtual void cancel_active_request() = 0;
    virtual void reset_session() = 0;
    [[nodiscard]] virtual std::size_t session_entries() const noexcept = 0;
};
```

---

## 2. Supported Provider Types

The `ProviderType` enumeration identifies backends:

```cpp
enum class ProviderType {
    none,       ///< No provider selected
    deepseek,   ///< DeepSeek API backend
    gemini,     ///< Google Gemini API backend
    openrouter, ///< OpenRouter multi-model gateway backend
    custom      ///< Custom or mock provider implementation
};
```

### Factory Function

Use `create_provider()` to instantiate built-in providers:

```cpp
#include <arn/core/provider/model_provider.hpp>

std::unique_ptr<arn::core::IModelProvider> prov =
    arn::core::create_provider(arn::core::ProviderType::gemini);
```

### Type Helpers

```cpp
// Convert enum to display string ("Gemini", "DeepSeek", "OpenRouter", "Custom", "none")
std::string_view display_name = arn::core::provider_type_name(prov->type());

// Parse string identifier ("gemini", "deepseek", "openrouter", "custom")
arn::core::ProviderType t = arn::core::provider_type_from_name("deepseek");
```

---

## 3. Built-in Providers

### 3.1. Google Gemini (`GeminiProvider`)

Implements the official Google Gemini v1beta REST API with Server-Sent Events streaming.

- **Default Endpoint**: `https://generativelanguage.googleapis.com`
- **Streaming Path**: `/v1beta/models/{model}:streamGenerateContent?key={key}&alt=sse`
- **Model Discovery Path**: `/v1beta/models?key={key}`
- **Preferred Models**: `gemini-2.0-flash`, `gemini-1.5-pro`, `gemini-1.5-flash`

#### Wire Format Details
- Maintains internal history in a `contents_` array with roles `"user"` and `"model"`.
- Tool declarations are serialized under `tools[0].functionDeclarations`.
- Tool calls emitted by Gemini arrive in `part.functionCall` containing `name` and `args`.
- Tool responses are returned as `part.functionResponse` containing `name` and `response.content`.

#### Custom Endpoint Usage (e.g. Proxies or Enterprise Gateways)

```cpp
#include <arn/core/provider/gemini_provider.hpp>

// Route through custom endpoint
auto gemini = std::make_shared<arn::core::GeminiProvider>("https://my-gemini-proxy.corp.internal");
session.configure_provider(gemini, api_key);
```

---

### 3.2. DeepSeek (`DeepSeekProvider`)

Implements DeepSeek's OpenAI-compatible completions endpoint with streaming tool calls.

- **Default Endpoint**: `https://api.deepseek.com`
- **Completions Path**: `/chat/completions`
- **Model Discovery Path**: `/models`
- **Authentication**: `Authorization: Bearer <key>` header
- **Preferred Models**: `deepseek-chat`, `deepseek-reasoner`

#### Wire Format Details
- Maintains conversation history in a `messages_` array with roles `"system"`, `"user"`, `"assistant"`, and `"tool"`.
- Tools are declared in the standard OpenAI format (`tools: [{ type: "function", function: ... }]`).
- Parses incremental SSE chunks (`data: {"choices": [{"delta": {"content": "...", "tool_calls": [...]}}]}`).
- Matches tool results back to the model using `tool_call_id`.

#### Custom Endpoint Usage

```cpp
#include <arn/core/provider/deepseek_provider.hpp>

auto deepseek = std::make_shared<arn::core::DeepSeekProvider>("https://api.deepseek.com");
session.configure_provider(deepseek, api_key);
```

---

### 3.3. OpenRouter (`OpenRouterProvider`)

Provides access to hundreds of LLMs (Claude, GPT-4o, Llama 3, Mistral, Command-R, etc.) through the OpenRouter gateway.

- **Default Endpoint**: `https://openrouter.ai`
- **API Prefix**: `/api/v1`
- **Completions Path**: `/api/v1/chat/completions`
- **Model Discovery Path**: `/api/v1/models`
- **Authentication**: `Authorization: Bearer <key>` header

#### Dynamic Tool Capability Discovery
OpenRouter hosts models with varying feature sets. During `list_models()`, `OpenRouterProvider` inspects each model's `supported_parameters` list in the model catalog:
- If a model supports `"tools"`, `model_capabilities(model).supports_tools` is marked `true`.
- If a model lacks tool calling, `supports_tools` is marked `false`, and `OpenRouterProvider` automatically omits tool schemas from requests to avoid server-side 400 Bad Request errors.

#### Configuration (`OpenRouterConfig`)

```cpp
#include <arn/core/provider/openrouter_provider.hpp>

arn::core::OpenRouterConfig config{
    .endpoint = "https://openrouter.ai",
    .api_path_prefix = "/api/v1",
    .http_referer = "https://github.com/ARN-Forge/ARN-Code", // Custom app URL for rankings
    .app_title = "ARN Code Assistant"                        // Application name
};

auto router = std::make_shared<arn::core::OpenRouterProvider>(config);
session.configure_provider(router, openrouter_key);
session.select_model("anthropic/claude-3.7-sonnet");
```

---

## 4. Comparison Matrix

| Feature | Gemini (`GeminiProvider`) | DeepSeek (`DeepSeekProvider`) | OpenRouter (`OpenRouterProvider`) |
|---|---|---|---|
| **Protocol** | Google Gemini v1beta | OpenAI-Compatible Chat | OpenAI-Compatible Gateway |
| **Authentication** | URL Query Parameter (`?key=`) | `Authorization: Bearer` | `Authorization: Bearer` |
| **Streaming Wire Format** | SSE (`data: {"candidates": ...}`) | SSE (`data: {"choices": ...}`) | SSE (`data: {"choices": ...}`) |
| **Tool Calling Wire Format** | `functionDeclarations` | `tools` / `tool_calls` | `tools` / `tool_calls` (auto-detected) |
| **App Identity Headers** | N/A | N/A | `HTTP-Referer`, `X-Title` |
| **History Structure** | `contents` (`user` / `model`) | `messages` (`system`, `user`, `assistant`, `tool`) | `messages` (`system`, `user`, `assistant`, `tool`) |
| **Default Models** | `gemini-2.0-flash`<br>`gemini-1.5-pro` | `deepseek-chat`<br>`deepseek-reasoner` | Hundreds across all major providers |

---

## 5. Session State & History Trimming

Each provider instance maintains the active multi-turn conversation in memory:
- **`session_entries()`**: Returns the total number of message turns currently recorded.
- **`reset_session()`**: Clears all conversation history without altering provider endpoints or credentials.
- **`trim_history(max_entries)`**: Prunes older messages while preserving context integrity:
  - System instructions are retained.
  - Paired tool calls and tool responses are preserved together so the model is not left with dangling tool executions.
  - Automatically called by `AgentSession::prompt()` according to `config.max_history_entries`.
