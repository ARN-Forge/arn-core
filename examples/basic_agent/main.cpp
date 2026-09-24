#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include <arn/core/agent/agent_session.hpp>
#include <arn/core/confirmation/confirmation_request.hpp>
#include <arn/core/provider/model_provider.hpp>
#include <arn/core/tool/tool.hpp>
#include <arn/core/tool/tool_registry.hpp>

// 1. Math calculation tool (safe, read-only)
class CalculatorTool final : public arn::core::ITool {
public:
    const arn::core::ToolDefinition& definition() const noexcept override {
        static const arn::core::ToolDefinition def{
            .name = "calculate",
            .description = "Performs arithmetic operations on two numbers (op: add, sub, mul, div).",
            .parameter_schema = {
                {"type", "object"},
                {"properties", {
                    {"a", {{"type", "number"}, {"description", "The first operand"}}},
                    {"b", {{"type", "number"}, {"description", "The second operand"}}},
                    {"op", {
                        {"type", "string"},
                        {"enum", {"add", "sub", "mul", "div"}},
                        {"description", "Arithmetic operation to perform"}
                    }}
                }},
                {"required", {"a", "b", "op"}}
            }
        };
        return def;
    }

    arn::core::ToolResult execute(const nlohmann::json& arguments,
                                  const arn::core::ToolContext& /*context*/) override {
        const double a = arguments.value("a", 0.0);
        const double b = arguments.value("b", 0.0);
        const std::string op = arguments.value("op", "add");

        double result = 0.0;
        if (op == "add") {
            result = a + b;
        } else if (op == "sub") {
            result = a - b;
        } else if (op == "mul") {
            result = a * b;
        } else if (op == "div") {
            if (b == 0.0) {
                return arn::core::ToolResult::failure("Division by zero is undefined.");
            }
            result = a / b;
        } else {
            return arn::core::ToolResult::failure("Unsupported operation: " + op);
        }

        return arn::core::ToolResult::success({{"result", result}});
    }
};

// 2. Sensitive action tool (requires human confirmation)
class SystemActionTool final : public arn::core::ITool {
public:
    const arn::core::ToolDefinition& definition() const noexcept override {
        static const arn::core::ToolDefinition def{
            .name = "execute_command",
            .description = "Executes an operating system maintenance task.",
            .parameter_schema = {
                {"type", "object"},
                {"properties", {
                    {"command", {{"type", "string"}, {"description", "The command to run"}}}
                }},
                {"required", {"command"}}
            }
        };
        return def;
    }

    bool requires_confirmation() const noexcept override {
        return true; // Demands approval before executing
    }

    arn::core::ToolResult execute(const nlohmann::json& arguments,
                                  const arn::core::ToolContext& context) override {
        const std::string command = arguments.value("command", "");

        // Request approval
        arn::core::ConfirmationRequest req(
            "execute_command",
            arguments,
            "Run command: " + command,
            /*changes_state=*/true,
            /*changes_files=*/false,
            /*preview=*/nlohmann::json{{"command", command}}
        );

        if (context.confirm && !context.confirm(req)) {
            return arn::core::ToolResult::failure("Command execution was rejected by the operator.");
        }

        return arn::core::ToolResult::success({
            {"status", "completed"},
            {"command", command}
        });
    }
};

// 3. Fallback mock provider for offline demonstration when no API keys are present
class DemoMockProvider final : public arn::core::IModelProvider {
public:
    arn::core::ProviderType type() const noexcept override { return arn::core::ProviderType::custom; }
    std::string_view name() const noexcept override { return "MockDemo"; }
    std::string preferred_model(const std::vector<std::string>& models) const override {
        return models.empty() ? "mock-demo-v1" : models.front();
    }

    arn::core::ApiResult list_models(const std::string&, const std::atomic_bool*) override {
        return {true, "Mock models available", {"mock-demo-v1"}};
    }

    arn::core::ModelTurn start_turn(
        const std::string&, const std::string&,
        const std::string&, const std::string& user_prompt,
        const arn::core::ToolRegistry&,
        const arn::core::StreamCallbacks& callbacks,
        const std::atomic_bool*) override {

        const std::string text = "I am a local mock agent running without network calls. You asked: \"" +
                                 user_prompt + "\". To query live models, set GEMINI_API_KEY, DEEPSEEK_API_KEY, or OPENROUTER_API_KEY.";
        if (callbacks.on_text) {
            callbacks.on_text(text);
        }
        return {.ok = true, .text = text};
    }

    arn::core::ModelTurn continue_turn(
        const std::string&, const std::string&,
        const std::string&,
        const std::vector<arn::core::ToolResponse>&,
        const arn::core::ToolRegistry&,
        const arn::core::StreamCallbacks&,
        const std::atomic_bool*) override {
        return {.ok = true, .text = "Mock tool turn completed."};
    }

    void trim_history(std::size_t) override {}
    void cancel_active_request() override {}
    void reset_session() override { turns_ = 0; }
    std::size_t session_entries() const noexcept override { return turns_; }

private:
    std::size_t turns_{0};
};

int main() {
    std::cout << "===========================================\n"
              << "       ARN Core Basic Agent Example        \n"
              << "===========================================\n\n";

    // Configure Agent Session
    arn::core::AgentConfig config{
        .system_instruction = "You are a helpful, precise engineering assistant. "
                              "Always use the calculate tool for math.",
        .max_tool_rounds = 6,
        .max_history_entries = 20
    };
    arn::core::AgentSession session(config);

    // Register Tools
    auto registry = std::make_shared<arn::core::ToolRegistry>();
    registry->register_tool(std::make_shared<CalculatorTool>());
    registry->register_tool(std::make_shared<SystemActionTool>());
    session.set_tools(registry);

    std::cout << "Registered " << registry->size() << " tools in registry:\n";
    for (const auto& def : registry->definitions()) {
        std::cout << "  - " << def.name << ": " << def.description << '\n';
    }
    std::cout << '\n';

    // Set interactive confirmation handler
    session.set_confirmation_handler([](const arn::core::ConfirmationRequest& req) -> bool {
        std::cout << "\n[CONFIRMATION REQUIRED]\n"
                  << "  Tool:    " << req.name << "\n"
                  << "  Summary: " << req.summary << "\n"
                  << "  Approve? (y/N): " << std::flush;
        std::string reply;
        if (!std::getline(std::cin, reply)) return false;
        return !reply.empty() && (reply[0] == 'y' || reply[0] == 'Y');
    });

    // Detect API Keys from environment
    const char* gemini_key = std::getenv("GEMINI_API_KEY");
    const char* deepseek_key = std::getenv("DEEPSEEK_API_KEY");
    const char* openrouter_key = std::getenv("OPENROUTER_API_KEY");

    if (gemini_key && *gemini_key) {
        std::cout << "Detected GEMINI_API_KEY. Configuring Gemini provider...\n";
        auto res = session.configure_provider(arn::core::ProviderType::gemini, gemini_key);
        if (!res.ok) {
            std::cerr << "Gemini configuration failed: " << res.message << '\n';
            return 1;
        }
    } else if (deepseek_key && *deepseek_key) {
        std::cout << "Detected DEEPSEEK_API_KEY. Configuring DeepSeek provider...\n";
        auto res = session.configure_provider(arn::core::ProviderType::deepseek, deepseek_key);
        if (!res.ok) {
            std::cerr << "DeepSeek configuration failed: " << res.message << '\n';
            return 1;
        }
    } else if (openrouter_key && *openrouter_key) {
        std::cout << "Detected OPENROUTER_API_KEY. Configuring OpenRouter provider...\n";
        auto res = session.configure_provider(arn::core::ProviderType::openrouter, openrouter_key);
        if (!res.ok) {
            std::cerr << "OpenRouter configuration failed: " << res.message << '\n';
            return 1;
        }
    } else {
        std::cout << "No API keys found in environment. Using DemoMockProvider (offline mode).\n";
        auto mock = std::make_shared<DemoMockProvider>();
        auto mock_res = session.configure_provider(mock, "dummy-mock-key");
        (void)mock_res;
    }

    std::cout << "Active Provider: " << arn::core::provider_type_name(session.active_provider()) << '\n'
              << "Active Model:    " << session.active_model() << "\n\n";

    // Streaming callbacks
    arn::core::StreamCallbacks callbacks{
        .on_text = [](std::string_view delta) {
            std::cout << delta << std::flush;
        },
        .on_progress = [](std::string_view msg) {
            std::cout << "\n[" << msg << "]\n";
        }
    };

    // Execute prompt
    const std::string prompt_text = "What is (128.5 * 4) + 17.25?";
    std::cout << "User: " << prompt_text << "\nAgent: ";

    auto result = session.prompt(prompt_text, callbacks);

    std::cout << "\n\n";
    if (!result.ok) {
        std::cerr << "Agent prompt failed: " << result.message << '\n';
        return 1;
    }

    std::cout << "Success! Completed turn with " << session.session_entries() << " recorded turns.\n";
    return 0;
}
