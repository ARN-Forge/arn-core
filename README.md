# ARN Core (`arn::core`)

[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.wikipedia.org/wiki/C%2B%2B23)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

**ARN Core** is a reusable, headless, modern C++23 framework for building autonomous AI agents and interactive LLM applications.

---

## What ARN Core Is

ARN Core provides the foundational orchestration engine for AI agent workflows:
- A clean, provider-agnostic agent orchestration layer (`AgentSession`).
- Pluggable model providers implementing a unified interface (`IModelProvider`).
- Generic tool registration, reflection, and invocation (`ToolRegistry`, `ITool`).
- Human-in-the-loop confirmation primitives (`ConfirmationGate`, `ConfirmationRequest`).
- Resilient streaming HTTP networking with Server-Sent Events (SSE) decoding and retry logic.

## What ARN Core Is NOT

- **Not an agent application CLI**: ARN Core contains no terminal interface, REPL, or Protocol 2 server.
- **Not a coding prompt or coding assistant**: ARN Core contains no built-in system prompt, no coding persona, and no coding-specific logic.
- **Not a filesystem sandbox**: ARN Core does not sandbox file access or provide file execution restrictions. Application consumers are responsible for defining their own tools and security enforcement.

---

## Architecture

```text
AgentSession
    │
    ├── IModelProvider
    │    ├── GeminiProvider
    │    ├── DeepSeekProvider
    │    └── OpenRouterProvider
    │
    ├── ToolRegistry
    │    └── ITool (user-defined tools)
    │
    └── ConfirmationGate / ConfirmationHandler
```

---

## Features

- **Modern C++23**: Concepts, designated initializers, `std::string_view`, standard library conveniences.
- **Provider-Independent Orchestration**: `AgentSession` manages multi-turn conversation history, executes tool loops, and streams tokens regardless of the backend provider.
- **Supported Providers**:
  - **Google Gemini** (`gemini-2.0-flash`, `gemini-1.5-pro`, etc.)
  - **DeepSeek** (`deepseek-chat`, `deepseek-reasoner`)
  - **OpenRouter** (unified gateway to Claude, Llama, Mistral, and more)
- **Streaming & Cancellation**: Real-time SSE token delivery with cooperative thread-safe cancellation via `std::atomic<bool>`.
- **Tool Calling**: Strict JSON Schema parameter definitions with automatic tool call extraction and dispatch.
- **Confirmation Abstraction**: Thread-safe human-in-the-loop approvals for dangerous or state-altering actions.
- **Resilient Networking**: Built-in exponential backoff, rate limit handling, and HTTP error classification.

---

## Basic Usage

Here is a minimal example using `arn::core`:

```cpp
#include <iostream>
#include <memory>
#include <arn/core/agent/agent_session.hpp>
#include <arn/core/provider/model_provider.hpp>
#include <arn/core/tool/tool_registry.hpp>

int main() {
    // 1. Configure the agent session
    arn::core::AgentConfig config{
        .system_instruction = "You are a helpful mathematical assistant.",
        .max_tool_rounds = 4,
        .max_history_entries = 10
    };
    arn::core::AgentSession session(config);

    // 2. Set up tool registry
    auto tools = std::make_shared<arn::core::ToolRegistry>();
    // Register custom tools implementing arn::core::ITool here
    session.set_tools(tools);

    // 3. Configure provider (Gemini, DeepSeek, or OpenRouter)
    auto provider = arn::core::create_provider(arn::core::ProviderType::gemini);
    session.set_provider(provider.get(), "YOUR_GEMINI_API_KEY");
    session.select_model("gemini-2.0-flash");

    // 4. Stream prompt response
    auto result = session.prompt("What is 42 * 1337?", [](std::string_view delta) {
        std::cout << delta << std::flush;
    });

    if (!result.success) {
        std::cerr << "\nError: " << result.error_message << '\n';
        return 1;
    }

    std::cout << '\n';
    return 0;
}
```

---

## CMake Integration

### Method 1: Installed Package (`find_package`)

Install `arn_core` to your system or a local prefix:

```bash
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/path/to/install
cmake --build build --config Release
cmake --install build --config Release
```

In your consumer `CMakeLists.txt`:

```cmake
find_package(arn_core 0.1.0 CONFIG REQUIRED)

add_executable(my_agent main.cpp)
target_link_libraries(my_agent PRIVATE arn::core)
```

### Method 2: FetchContent

```cmake
include(FetchContent)

FetchContent_Declare(
    arn_core
    GIT_REPOSITORY https://github.com/arnecto/arn-core.git
    GIT_TAG v0.1.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(arn_core)

add_executable(my_agent main.cpp)
target_link_libraries(my_agent PRIVATE arn::core)
```

### Method 3: Source Subdirectory (`add_subdirectory`)

```cmake
add_subdirectory(path/to/arn-core)

add_executable(my_agent main.cpp)
target_link_libraries(my_agent PRIVATE arn::core)
```

---

## Building and Testing

### Prerequisites

- C++23 compliant compiler:
  - MSVC 19.38+ (Visual Studio 2022 / 2026)
  - GCC 13+
  - Clang 17+
- CMake 3.20 or newer
- OpenSSL (required for HTTPS / SSL communication)

### Windows (MSVC)

```cmd
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

### Linux / macOS

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

All unit tests run offline using local loopback HTTP mock servers and dummy providers. No real API keys or network calls are required to run tests.

---

## Security Model & Boundary

It is vital to understand the boundary between **ARN Core** and **Applications**:

| Concern | ARN Core (`arn::core`) | Application Layer (e.g. ARN CLI) |
|---|---|---|
| **Tools Abstraction** | Generic `ITool`, `ToolRegistry`, schemas | Specific tool implementations (e.g. file editing, bash, search) |
| **Confirmation** | `ConfirmationGate`, `ConfirmationRequest` interface | Prompting user in UI, interactive approval, diff display |
| **Filesystem Safety** | None | Sandboxing, junction escape prevention, path protection |
| **Workspace Policy** | None | Read-only vs modifying distinctions, root enforcement |

> [!WARNING]
> ARN Core does **not** sanitize filesystem paths or restrict OS command execution. Security policies must be enforced within the concrete `ITool` implementations provided by the host application.

---

## Relationship to ARN

ARN Core was originally developed as the foundation for **ARN** (`arnecto/arn`), an autonomous coding assistant for the terminal. During Phase 1–6 architectural refactoring, the agent orchestration engine, provider integrations, and tool abstractions were completely decoupled into this standalone repository.

ARN is now a consumer of ARN Core. Future applications (such as ARNday or custom domain agents) can consume `arn::core` without depending on ARN's CLI or coding prompt.

---

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE) for details.
