# ARN Core (`arn::core`)

[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.wikipedia.org/wiki/C%2B%2B23)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Tests: 6/6 Passing](https://img.shields.io/badge/Tests-6%2F6%20Passing-brightgreen.svg)]()

**ARN Core** is a reusable, headless, modern C++23 orchestration framework for building autonomous AI agents and interactive LLM applications.

---

## What ARN Core Is

ARN Core provides the foundational orchestration engine for AI agent workflows:
- **Clean Agent Orchestration**: High-level multi-turn session coordinator (`AgentSession`).
- **Pluggable Model Providers**: Unified provider abstraction (`IModelProvider`) with built-in integrations for **Google Gemini**, **DeepSeek**, and **OpenRouter**.
- **Schema-Driven Tools**: Type-safe tool definition using JSON Schema, tool discovery, and dynamic invocation (`ToolRegistry`, `ITool`).
- **Human-in-the-Loop Approvals**: Primitives for synchronous and asynchronous user confirmation gates (`ConfirmationGate`, `ConfirmationRequest`).
- **Resilient Networking**: Server-Sent Events (SSE) streaming with exponential backoff, rate-limit handling, duplicate replay prevention, and non-blocking cooperative cancellation.

## What ARN Core Is NOT

- **Not an agent CLI**: ARN Core contains no terminal interface, REPL, or Protocol 2 server.
- **Not a coding prompt or coding assistant**: ARN Core contains no hardcoded persona or coding-specific prompt templates.
- **Not a filesystem sandbox**: ARN Core does not restrict OS commands or sandbox file paths. Host applications implement domain-specific tools and enforce security boundaries.

---

## Documentation Index

Comprehensive developer guides are available in the [`docs/`](docs/) directory:

- [**Getting Started**](docs/getting-started.md) — Prerequisites, build integration, and your first streaming agent.
- [**Architecture Overview**](docs/architecture.md) — Subsystems, data flow, concurrency model, and security boundaries.
- [**AgentSession Guide**](docs/agent-session.md) — Session configuration, model selection, multi-turn history, and cancellation.
- [**Model Providers**](docs/providers.md) — Google Gemini, DeepSeek, OpenRouter, capability detection, and wire formats.
- [**Tool System**](docs/tools.md) — `ITool` interface, JSON Schema design, error containment, and `ToolRegistry`.
- [**Human-in-the-Loop Confirmations**](docs/confirmations.md) — `ConfirmationRequest`, CLI prompts, and asynchronous `ConfirmationGate`.
- [**Streaming & Cancellation**](docs/streaming-and-cancellation.md) — Token streaming via `SseDecoder`, network retries, and non-blocking aborts.
- [**Writing a Custom Tool**](docs/custom-tool.md) — Step-by-step tutorial for domain-specific tools.
- [**Writing a Custom Provider**](docs/custom-provider.md) — Step-by-step guide for local runtimes (Ollama, llama.cpp, vLLM).

---

## Architecture

```text
AgentSession
    │
    ├── IModelProvider
    │    ├── GeminiProvider (v1beta REST / SSE)
    │    ├── DeepSeekProvider (OpenAI-compatible / SSE)
    │    └── OpenRouterProvider (Unified multi-model gateway)
    │
    ├── ToolRegistry
    │    └── ITool (user-defined tools with JSON Schema)
    │
    └── ConfirmationGate / ConfirmationHandler
         └── ConfirmationRequest (Human-in-the-loop approvals)
```

---

## Quick Example

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

    if (!result.ok) {
        std::cerr << "\nError: " << result.message << '\n';
        return 1;
    }

    std::cout << '\n';
    return 0;
}
```

A complete, runnable example with tool execution and confirmation handling is available under [`examples/basic_agent/`](examples/basic_agent/).

---

## CMake Integration

### Method 1: Installed Package (`find_package`) — Recommended

Install `arn_core` to your system or local prefix:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/path/to/install
cmake --build build --config Release
cmake --install build --config Release
```

In your consumer `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_agent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(arn_core 0.1.0 CONFIG REQUIRED)

add_executable(my_agent main.cpp)
target_link_libraries(my_agent PRIVATE arn::core)
```

### Method 2: FetchContent

```cmake
include(FetchContent)

FetchContent_Declare(
    arn_core
    GIT_REPOSITORY https://github.com/ARN-Forge/arn-core.git
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
- OpenSSL (required for HTTPS / TLS communication)

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

All 6 unit tests run 100% offline using local loopback HTTP mock servers and dummy providers. No external network access or API keys are required.

---

## Security Model & Boundary

| Concern | ARN Core (`arn::core`) | Application Layer (e.g. ARN CLI) |
|---|---|---|
| **Tools Abstraction** | Generic `ITool`, `ToolRegistry`, schemas | Specific tool implementations (e.g. file editing, bash, search) |
| **Confirmation** | `ConfirmationGate`, `ConfirmationRequest` interface | Prompting user in UI, interactive approval, diff display |
| **Filesystem Safety** | None | Sandboxing, junction escape prevention, path protection |
| **Workspace Policy** | None | Read-only vs modifying distinctions, root enforcement |

> [!WARNING]
> ARN Core does **not** sanitize filesystem paths or restrict OS command execution. Security policies must be enforced within the concrete `ITool` implementations provided by the host application.

---

## Relationship to the ARN Ecosystem

ARN Core was originally developed as the orchestration engine for **[ARN Code](https://github.com/ARN-Forge/ARN-Code)**, an autonomous terminal coding assistant. During architectural refactoring, the agent orchestration engine, provider integrations, and tool abstractions were completely decoupled into this standalone repository.

ARN Code is now a consumer of ARN Core. Future applications (desktop assistants, domain agents, server daemons) can consume `arn::core` directly.

---

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE) for details.
