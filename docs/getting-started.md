# Getting Started with ARN Core

Welcome to **ARN Core** (`arn::core`), a lightweight, modular C++23 orchestration framework for building autonomous AI agents and interactive language model applications.

This guide walks you through system prerequisites, integrating the library with CMake, configuring your first model provider, registering a tool, and streaming agent responses.

---

## 1. Prerequisites

ARN Core requires a modern C++23 toolchain and OpenSSL for encrypted HTTPS/TLS networking.

### Supported Compilers
- **MSVC**: Visual Studio 2022 (v19.38+) or Visual Studio 2026 with `/std:c++latest` or `/std:c++23`
- **GCC**: Version 13.0 or newer with `-std=c++23`
- **Clang**: Version 17.0 or newer with `-std=c++23`

### Build System & Dependencies
- **CMake**: Version 3.20 or newer
- **OpenSSL**: Version 1.1.1 or 3.x (development headers and libraries)
  - Windows: Available via vcpkg (`vcpkg install openssl:x64-windows`) or pre-built binaries
  - Ubuntu/Debian: `sudo apt-get install libssl-dev`
  - macOS: `brew install openssl`

> [!NOTE]
> Third-party dependencies ([nlohmann_json](https://github.com/nlohmann/json) and [cpp-httplib](https://github.com/yhirose/cpp-httplib)) are automatically discovered if present on your system, or fetched transparently via CMake `FetchContent`.

---

## 2. CMake Integration

You can integrate ARN Core into your project using one of three standard approaches.

### Method A: Installed Package (`find_package`) — Recommended

First, build and install ARN Core to your desired prefix:

```bash
# Configure and build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/path/to/install
cmake --build build --config Release

# Install
cmake --install build --config Release
```

Then in your application's `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_agent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find the installed package
find_package(arn_core 0.1.0 CONFIG REQUIRED)

add_executable(my_agent main.cpp)

# Link against the imported target
target_link_libraries(my_agent PRIVATE arn::core)
```

### Method B: CMake FetchContent

To fetch and compile ARN Core directly from source in your CMake project:

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

### Method C: Subdirectory (`add_subdirectory`)

If you maintain ARN Core as a Git submodule or local vendor directory:

```cmake
add_subdirectory(vendor/arn-core)

add_executable(my_agent main.cpp)
target_link_libraries(my_agent PRIVATE arn::core)
```

---

## 3. Your First Agent

Here is a complete, working example demonstrating how to configure `arn::core::AgentSession`, attach a custom tool, stream the response, and handle errors.

### `main.cpp`

```cpp
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include <arn/core/agent/agent_session.hpp>
#include <arn/core/provider/model_provider.hpp>
#include <arn/core/tool/tool.hpp>
#include <arn/core/tool/tool_registry.hpp>

// 1. Define a custom calculation tool implementing arn::core::ITool
class CalculatorTool final : public arn::core::ITool {
public:
    const arn::core::ToolDefinition& definition() const noexcept override {
        static const arn::core::ToolDefinition def{
            .name = "add_numbers",
            .description = "Adds two floating point numbers together.",
            .parameter_schema = {
                {"type", "object"},
                {"properties", {
                    {"a", {{"type", "number"}, {"description", "The first addend"}}},
                    {"b", {{"type", "number"}, {"description", "The second addend"}}}
                }},
                {"required", {"a", "b"}}
            }
        };
        return def;
    }

    arn::core::ToolResult execute(const nlohmann::json& arguments,
                                  const arn::core::ToolContext& /*context*/) override {
        if (!arguments.contains("a") || !arguments.contains("b")) {
            return arn::core::ToolResult::failure("Missing required arguments 'a' or 'b'.");
        }

        const double a = arguments["a"].get<double>();
        const double b = arguments["b"].get<double>();
        const double sum = a + b;

        return arn::core::ToolResult::success({{"sum", sum}});
    }
};

int main() {
    // 2. Read API key from environment
    const char* env_key = std::getenv("GEMINI_API_KEY");
    if (!env_key || std::string_view(env_key).empty()) {
        std::cerr << "Error: Please set the GEMINI_API_KEY environment variable.\n";
        return 1;
    }

    // 3. Configure the session
    arn::core::AgentConfig config{
        .system_instruction = "You are a concise assistant equipped with tools. "
                              "Always use available tools for arithmetic.",
        .max_tool_rounds = 5,
        .max_history_entries = 20
    };
    arn::core::AgentSession session(config);

    // 4. Register tools
    auto registry = std::make_shared<arn::core::ToolRegistry>();
    registry->register_tool(std::make_shared<CalculatorTool>());
    session.set_tools(registry);

    // 5. Configure provider (Gemini, DeepSeek, or OpenRouter)
    std::cout << "Connecting to Gemini provider...\n";
    auto config_res = session.configure_provider(arn::core::ProviderType::gemini, env_key);
    if (!config_res.ok) {
        std::cerr << "Provider setup failed: " << config_res.message << "\n";
        return 1;
    }

    std::cout << "Active model: " << session.active_model() << "\n\n";

    // 6. Define streaming callbacks
    arn::core::StreamCallbacks callbacks{
        .on_text = [](std::string_view delta) {
            std::cout << delta << std::flush;
        },
        .on_progress = [](std::string_view progress) {
            std::cout << "\n[" << progress << "]\n";
        }
    };

    // 7. Prompt the agent
    std::cout << "User: What is 1984.75 plus 42.25?\nAgent: ";
    auto result = session.prompt("What is 1984.75 plus 42.25?", callbacks);

    std::cout << "\n\n";

    if (!result.ok) {
        std::cerr << "Prompt failed: " << result.message << "\n";
        return 1;
    }

    std::cout << "Turn finished successfully. Session entries: "
              << session.session_entries() << "\n";
    return 0;
}
```

---

## 4. Next Steps

Explore the rest of the ARN Core developer documentation:
- [Architecture Overview](architecture.md): Understand the decoupled design and threading model.
- [AgentSession Guide](agent-session.md): Master session configuration, multi-turn state, and history trimming.
- [Model Providers](providers.md): Configure Gemini, DeepSeek, OpenRouter, and custom endpoints.
- [Tool System](tools.md): Author JSON Schemas, handle tool results, and manage registry lifecycles.
- [Confirmations](confirmations.md): Implement interactive human-in-the-loop approvals.
- [Streaming & Cancellation](streaming-and-cancellation.md): Real-time token streaming and cooperative task aborts.
- [Writing a Custom Tool](custom-tool.md): Step-by-step tutorial for domain-specific tools.
- [Writing a Custom Provider](custom-provider.md): Tutorial for embedding custom/local LLM runtimes (Ollama, llama.cpp).
