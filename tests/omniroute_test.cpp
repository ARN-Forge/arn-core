#include "arn/core/agent/agent_session.hpp"
#include "arn/core/provider/model_provider.hpp"
#include "arn/core/provider/omniroute_provider.hpp"
#include "arn/core/tool/tool_registry.hpp"
#include "providers/provider_utils.hpp"

#include <httplib.h>

#include <atomic>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

class AddTool final : public arn::core::ITool {
public:
    const arn::core::ToolDefinition& definition() const noexcept override {
        static const arn::core::ToolDefinition def{
            "add",
            "Add two numbers",
            {
                {"type", "object"},
                {"properties", {
                    {"a", {{"type", "number"}}},
                    {"b", {{"type", "number"}}}
                }},
                {"required", {"a", "b"}}
            }
        };
        return def;
    }

    arn::core::ToolResult execute(const nlohmann::json& arguments,
                                  const arn::core::ToolContext&) override {
        const double a = arguments.value("a", 0.0);
        const double b = arguments.value("b", 0.0);
        return arn::core::ToolResult::success({{"sum", a + b}});
    }
};

class MultiplyTool final : public arn::core::ITool {
public:
    const arn::core::ToolDefinition& definition() const noexcept override {
        static const arn::core::ToolDefinition def{
            "multiply",
            "Multiply two numbers",
            {
                {"type", "object"},
                {"properties", {
                    {"a", {{"type", "number"}}},
                    {"b", {{"type", "number"}}}
                }},
                {"required", {"a", "b"}}
            }
        };
        return def;
    }

    arn::core::ToolResult execute(const nlohmann::json& arguments,
                                  const arn::core::ToolContext&) override {
        const double a = arguments.value("a", 0.0);
        const double b = arguments.value("b", 0.0);
        return arn::core::ToolResult::success({{"product", a * b}});
    }
};

} // namespace

#include "omniroute_hardening.hpp"

int main() {
    try {
        ScopedOmniEnvironment environment;
        run_omniroute_hardening_tests();
        std::cout << "Starting OmniRoute provider tests...\n";

        // 1. Factory creation and metadata
        {
            auto provider = arn::core::create_provider(arn::core::ProviderType::omniroute);
            check(provider != nullptr, "OmniRoute provider creation failed");
            check(provider->type() == arn::core::ProviderType::omniroute, "type() mismatch");
            check(provider->kind() == arn::core::ProviderType::omniroute, "kind() mismatch");
            check(provider->name() == "OmniRoute", "name() mismatch");
            check(provider->session_entries() == 0, "initial session_entries mismatch");
        }

        // 2. Provider name & enum conversions
        {
            check(arn::core::provider_type_name(arn::core::ProviderType::omniroute) == "OmniRoute",
                  "provider_type_name(omniroute)");
            check(arn::core::provider_type_from_name("omniroute") == arn::core::ProviderType::omniroute,
                  "provider_type_from_name(omniroute)");
            check(arn::core::provider_type_from_name("omni-route") == arn::core::ProviderType::omniroute,
                  "provider_type_from_name(omni-route)");
            check(arn::core::provider_type_from_name("omni_route") == arn::core::ProviderType::omniroute,
                  "provider_type_from_name(omni_route)");
        }

        // 3. Preferred model selection logic
        {
            arn::core::OmniRouteProvider provider;
            check(provider.preferred_model({"kiro", "other"}) == "kiro", "Preferred kiro");
            check(provider.preferred_model({"anthropic/claude-3.7-sonnet", "openai/gpt-4o"}) ==
                  "anthropic/claude-3.7-sonnet", "Preferred claude 3.7");
            check(provider.preferred_model({"custom-kiro-model", "other"}) == "custom-kiro-model",
                  "Preferred substring kiro");
            check(provider.preferred_model({"unknown-model"}) == "unknown-model",
                  "Preferred fallback");
            check(provider.preferred_model({}).empty(), "Preferred empty");
        }

        // 4. Base URL configuration and parse_base_url tests
        {
            arn::core::OmniRouteProvider provider;
            check(provider.config().endpoint == "http://localhost:20128", "Default endpoint");
            check(provider.config().api_path_prefix == "/v1", "Default prefix");
            check(provider.base_url() == "http://localhost:20128/v1", "Default base_url");

            provider.set_base_url("http://localhost:9090/custom/v1");
            check(provider.config().endpoint == "http://localhost:9090", "Custom endpoint");
            check(provider.config().api_path_prefix == "/custom/v1", "Custom prefix");
            check(provider.base_url() == "http://localhost:9090/custom/v1", "Custom base_url");

            provider.set_base_url("https://api.omniroute.ai");
            check(provider.config().endpoint == "https://api.omniroute.ai", "Domain endpoint");
            check(provider.config().api_path_prefix == "/v1", "Default fallback prefix");
            check(provider.base_url() == "https://api.omniroute.ai/v1", "Domain base_url");

            // parse_base_url directly
            auto [e1, p1] = arn::core::parse_base_url("http://127.0.0.1:8000", "/v1");
            check(e1 == "http://127.0.0.1:8000" && p1 == "/v1", "parse_base_url standard");

            auto [e2, p2] = arn::core::parse_base_url("http://127.0.0.1:8000/", "/v1");
            check(e2 == "http://127.0.0.1:8000" && p2 == "/v1", "parse_base_url trailing slash");

            auto [e3, p3] = arn::core::parse_base_url("  https://route.example.com/api/v2/  ", "/v1");
            check(e3 == "https://route.example.com" && p3 == "/api/v2", "parse_base_url trimmed");

            bool rejected = false;
            try { (void)arn::core::parse_base_url("", "/v1"); }
            catch (const std::invalid_argument&) { rejected = true; }
            check(rejected, "Empty custom endpoint must not silently fall back elsewhere");
        }

        // 5. Cancellation behavior without network
        {
            arn::core::OmniRouteProvider provider;
            std::atomic_bool cancel{true};
            auto res = provider.list_models("test-key", &cancel);
            check(!res.ok && res.cancelled, "Cancelled list_models");

            arn::core::ToolRegistry tools;
            auto turn = provider.start_turn("key", "test-model", "Sys", "Prompt", tools, {}, &cancel);
            check(!turn.ok && turn.cancelled, "Cancelled start_turn");

            auto turn2 = provider.continue_turn("key", "test-model", "Sys", {}, tools, {}, &cancel);
            check(!turn2.ok && turn2.cancelled, "Cancelled continue_turn");
        }

        // 6. Session reset and trimming
        {
            arn::core::OmniRouteProvider provider;
            check(provider.session_entries() == 0, "Initial entries");
            provider.trim_history(10);
            check(provider.session_entries() == 0, "Trim empty");
            provider.reset_session();
            check(provider.session_entries() == 0, "Reset empty");
        }

        // 7. End-to-end loopback mock HTTP server with AgentSession
        {
            std::cout << "Running loopback server test with AgentSession and OmniRoute...\n";
            httplib::Server mock_server;
            int turn_count = 0;

            mock_server.Get("/v1/models", [](const httplib::Request&, httplib::Response& res) {
                res.set_content(R"({
                    "data": [
                        {
                            "id": "kiro",
                            "name": "Kiro via OmniRoute",
                            "supported_parameters": ["tools", "temperature"]
                        },
                        {
                            "id": "anthropic/claude-3.7-sonnet",
                            "name": "Claude 3.7 Sonnet"
                        }
                    ]
                })", "application/json");
            });

            mock_server.Post("/v1/chat/completions", [&](const httplib::Request& req, httplib::Response& res) {
                ++turn_count;
                res.set_header("Content-Type", "text/event-stream");
                res.set_header("Cache-Control", "no-cache");

                if (turn_count == 1) {
                    // Turn 1: Model requests tool call to add(15, 27)
                    std::string chunk1 = "data: {\"choices\": [{\"delta\": {\"tool_calls\": [{\"index\": 0, \"id\": \"call_omni1\", \"type\": \"function\", \"function\": {\"name\": \"add\", \"arguments\": \"{\\\"a\\\": 15, \\\"b\\\": 27}\"}}]}}]}\n\n";
                    std::string chunk2 = "data: [DONE]\n\n";
                    res.set_content(chunk1 + chunk2, "text/event-stream");
                } else {
                    // Turn 2: Model receives tool result and produces final answer
                    std::string chunk1 = "data: {\"choices\": [{\"delta\": {\"content\": \"The calculated sum is 42.\"}}]}\n\n";
                    std::string chunk2 = "data: [DONE]\n\n";
                    res.set_content(chunk1 + chunk2, "text/event-stream");
                }
            });

            const int port = mock_server.bind_to_any_port("127.0.0.1");
            check(port > 0, "Mock server failed to bind port");

            std::thread server_thread([&] { mock_server.listen_after_bind(); });

            try {
                arn::core::OmniRouteConfig cfg{
                    .endpoint = "http://127.0.0.1:" + std::to_string(port),
                    .api_path_prefix = "/v1"
                };

                auto provider = std::make_shared<arn::core::OmniRouteProvider>(cfg);
                check(provider->type() == arn::core::ProviderType::omniroute, "Provider type");

                auto tools = std::make_shared<arn::core::ToolRegistry>();
                tools->register_tool(std::make_shared<AddTool>());

                arn::core::AgentSession session;
                session.set_tools(tools);
                session.set_system_instruction("You are OmniRoute assistant.");

                auto config_res = session.configure_provider(provider, "fake-omniroute-key");
                check(config_res.ok, "Configure OmniRoute provider through AgentSession");
                check(session.active_provider() == arn::core::ProviderType::omniroute, "Active provider");
                check(session.active_model() == "kiro", "Preferred model selected kiro");

                std::string streamed_result;
                auto prompt_res = session.prompt("Calculate 15 + 27",
                                                 [&](std::string_view delta) { streamed_result += delta; });

                check(prompt_res.ok, "Prompt round failed");
                check(prompt_res.message == "The calculated sum is 42.", "Prompt final message");
                check(streamed_result == "The calculated sum is 42.", "Streamed text mismatch");
                check(turn_count == 2, "AgentSession must have performed 2 turns");

                mock_server.stop();
                if (server_thread.joinable())
                    server_thread.join();
            } catch (...) {
                mock_server.stop();
                if (server_thread.joinable())
                    server_thread.join();
                throw;
            }
        }

        // 8. Streamed tool-call ID split across deltas with empty/null protection
        {
            std::string text;
            nlohmann::json tool_calls = nlohmann::json::array();

            const std::string chunk1 = R"({
                "choices": [{
                    "delta": {
                        "tool_calls": [{
                            "index": 0,
                            "id": "call_part1_",
                            "type": "function",
                            "function": {"name": "add", "arguments": ""}
                        }]
                    }
                }]
            })";
            const std::string chunk2 = R"({
                "choices": [{
                    "delta": {
                        "tool_calls": [{
                            "index": 0,
                            "id": "part2_xyz",
                            "function": {"arguments": "{\"a\": 1"}
                        }]
                    }
                }]
            })";
            const std::string chunk3 = R"({
                "choices": [{
                    "delta": {
                        "tool_calls": [{
                            "index": 0,
                            "id": "",
                            "function": {"name": "", "arguments": ", \"b\": 2}"}
                        }]
                    }
                }]
            })";
            const std::string chunk4 = R"({
                "choices": [{
                    "delta": {
                        "tool_calls": [{
                            "index": 0,
                            "id": null,
                            "function": {"name": null}
                        }]
                    }
                }]
            })";

            arn::core::detail::parse_openrouter_stream_chunk(chunk1, text, tool_calls, nullptr);
            arn::core::detail::parse_openrouter_stream_chunk(chunk2, text, tool_calls, nullptr);
            arn::core::detail::parse_openrouter_stream_chunk(chunk3, text, tool_calls, nullptr);
            arn::core::detail::parse_openrouter_stream_chunk(chunk4, text, tool_calls, nullptr);

            check(tool_calls.size() == 1, "Expected 1 aggregated tool call");
            check(tool_calls[0].value("id", "") == "call_part1_part2_xyz", "Tool ID accumulated across deltas");
            check(tool_calls[0]["function"].value("name", "") == "add", "Function name preserved");
            check(tool_calls[0]["function"].value("arguments", "") == "{\"a\": 1, \"b\": 2}", "Arguments accumulated");
        }

        // 9. Function name and arguments split across several deltas
        {
            std::string text;
            nlohmann::json tool_calls = nlohmann::json::array();

            const std::string chunk1 = R"({
                "choices": [{
                    "delta": {
                        "tool_calls": [{
                            "index": 0,
                            "id": "call_fn_split",
                            "function": {"name": "multi"}
                        }]
                    }
                }]
            })";
            const std::string chunk2 = R"({
                "choices": [{
                    "delta": {
                        "tool_calls": [{
                            "index": 0,
                            "function": {"name": "ply", "arguments": "{\"a\":"}
                        }]
                    }
                }]
            })";
            const std::string chunk3 = R"({
                "choices": [{
                    "delta": {
                        "tool_calls": [{
                            "index": 0,
                            "function": {"arguments": " 7, \"b\":"}
                        }]
                    }
                }]
            })";
            const std::string chunk4 = R"({
                "choices": [{
                    "delta": {
                        "tool_calls": [{
                            "index": 0,
                            "function": {"arguments": " 6}"}
                        }]
                    }
                }]
            })";

            arn::core::detail::parse_openrouter_stream_chunk(chunk1, text, tool_calls, nullptr);
            arn::core::detail::parse_openrouter_stream_chunk(chunk2, text, tool_calls, nullptr);
            arn::core::detail::parse_openrouter_stream_chunk(chunk3, text, tool_calls, nullptr);
            arn::core::detail::parse_openrouter_stream_chunk(chunk4, text, tool_calls, nullptr);

            check(tool_calls.size() == 1, "Expected 1 tool call");
            check(tool_calls[0].value("id", "") == "call_fn_split", "Tool ID");
            check(tool_calls[0]["function"].value("name", "") == "multiply", "Function name accumulated");
            check(tool_calls[0]["function"].value("arguments", "") == "{\"a\": 7, \"b\": 6}", "Arguments accumulated");
        }

        // 10. Multiple tool calls distinguished by index with interleaved chunks
        {
            std::string text;
            nlohmann::json tool_calls = nlohmann::json::array();

            const std::string chunk1 = R"({
                "choices": [{
                    "delta": {
                        "tool_calls": [
                            {"index": 0, "id": "call_0_", "function": {"name": "ad", "arguments": "{\"a\":"}},
                            {"index": 1, "id": "call_1_", "function": {"name": "multi", "arguments": "{\"a\":"}}
                        ]
                    }
                }]
            })";
            const std::string chunk2 = R"({
                "choices": [{
                    "delta": {
                        "tool_calls": [
                            {"index": 0, "id": "first", "function": {"name": "d", "arguments": " 10, \"b\": 20}"}},
                            {"index": 1, "id": "second", "function": {"name": "ply", "arguments": " 3, \"b\": 4}"}}
                        ]
                    }
                }]
            })";

            arn::core::detail::parse_openrouter_stream_chunk(chunk1, text, tool_calls, nullptr);
            arn::core::detail::parse_openrouter_stream_chunk(chunk2, text, tool_calls, nullptr);

            check(tool_calls.size() == 2, "Expected 2 independent tool calls");
            check(tool_calls[0].value("id", "") == "call_0_first", "Call 0 ID");
            check(tool_calls[0]["function"].value("name", "") == "add", "Call 0 Name");
            check(tool_calls[0]["function"].value("arguments", "") == "{\"a\": 10, \"b\": 20}", "Call 0 Arguments");

            check(tool_calls[1].value("id", "") == "call_1_second", "Call 1 ID");
            check(tool_calls[1]["function"].value("name", "") == "multiply", "Call 1 Name");
            check(tool_calls[1]["function"].value("arguments", "") == "{\"a\": 3, \"b\": 4}", "Call 1 Arguments");
        }

        // 11. Multi-step sequential tool use with fragmented deltas and outgoing request payload inspection
        {
            std::cout << "Running multi-step sequential tool use test with payload inspection...\n";
            httplib::Server mock_server;
            int turn_count = 0;
            std::vector<nlohmann::json> captured_payloads;
            std::mutex payload_mutex;

            mock_server.Get("/v1/models", [](const httplib::Request&, httplib::Response& res) {
                res.set_content(R"({
                    "data": [
                        {
                            "id": "kiro",
                            "name": "Kiro via OmniRoute",
                            "supported_parameters": ["tools", "temperature"]
                        }
                    ]
                })", "application/json");
            });

            mock_server.Post("/v1/chat/completions", [&](const httplib::Request& req, httplib::Response& res) {
                {
                    std::lock_guard lock(payload_mutex);
                    captured_payloads.push_back(nlohmann::json::parse(req.body));
                }
                ++turn_count;
                res.set_header("Content-Type", "text/event-stream");
                res.set_header("Cache-Control", "no-cache");

                if (turn_count == 1) {
                    // Turn 1: Model requests add(15, 27) with ID, name and args fragmented across deltas
                    std::string c1 = "data: {\"choices\": [{\"delta\": {\"tool_calls\": [{\"index\": 0, \"id\": \"call_add_\", \"type\": \"function\", \"function\": {\"name\": \"ad\"}}]}}]}\n\n";
                    std::string c2 = "data: {\"choices\": [{\"delta\": {\"tool_calls\": [{\"index\": 0, \"id\": \"step1\", \"function\": {\"name\": \"d\", \"arguments\": \"{\\\"a\\\": 15,\"}}]}}]}\n\n";
                    std::string c3 = "data: {\"choices\": [{\"delta\": {\"tool_calls\": [{\"index\": 0, \"id\": \"\", \"function\": {\"name\": \"\", \"arguments\": \" \\\"b\\\": 27}\"}}]}}]}\n\n";
                    std::string c4 = "data: {\"choices\": [{\"delta\": {\"tool_calls\": [{\"index\": 0, \"id\": null, \"function\": {\"name\": null}}]}}]}\n\n";
                    std::string c5 = "data: [DONE]\n\n";
                    res.set_content(c1 + c2 + c3 + c4 + c5, "text/event-stream");
                } else if (turn_count == 2) {
                    // Turn 2: Model requests multiply(42, 2) based on tool 1 result, with fragmented deltas
                    std::string c1 = "data: {\"choices\": [{\"delta\": {\"tool_calls\": [{\"index\": 0, \"id\": \"call_mul_\", \"type\": \"function\", \"function\": {\"name\": \"multiply\", \"arguments\": \"{\\\"a\\\": 42,\"}}]}}]}\n\n";
                    std::string c2 = "data: {\"choices\": [{\"delta\": {\"tool_calls\": [{\"index\": 0, \"id\": \"step2\", \"function\": {\"arguments\": \" \\\"b\\\": 2}\"}}]}}]}\n\n";
                    std::string c3 = "data: [DONE]\n\n";
                    res.set_content(c1 + c2 + c3, "text/event-stream");
                } else if (turn_count == 3) {
                    // Turn 3: Model receives tool 2 result and returns final answer
                    std::string c1 = "data: {\"choices\": [{\"delta\": {\"content\": \"The final calculation is 84.\"}}]}\n\n";
                    std::string c2 = "data: [DONE]\n\n";
                    res.set_content(c1 + c2, "text/event-stream");
                } else {
                    // Turn 4: Follow-up user turn
                    std::string c1 = "data: {\"choices\": [{\"delta\": {\"content\": \"Follow-up completed successfully.\"}}]}\n\n";
                    std::string c2 = "data: [DONE]\n\n";
                    res.set_content(c1 + c2, "text/event-stream");
                }
            });

            const int port = mock_server.bind_to_any_port("127.0.0.1");
            check(port > 0, "Mock server failed to bind port");

            std::thread server_thread([&] { mock_server.listen_after_bind(); });

            try {
                arn::core::OmniRouteConfig cfg{
                    .endpoint = "http://127.0.0.1:" + std::to_string(port),
                    .api_path_prefix = "/v1"
                };

                auto provider = std::make_shared<arn::core::OmniRouteProvider>(cfg);
                auto tools = std::make_shared<arn::core::ToolRegistry>();
                tools->register_tool(std::make_shared<AddTool>());
                tools->register_tool(std::make_shared<MultiplyTool>());

                arn::core::AgentSession session;
                session.set_tools(tools);
                session.set_system_instruction("You are a multi-step assistant.");

                auto config_res = session.configure_provider(provider, "fake-omniroute-key");
                check(config_res.ok, "Configure OmniRoute provider");

                std::string streamed_result;
                auto prompt_res = session.prompt("Calculate (15 + 27) * 2",
                                                 [&](std::string_view delta) { streamed_result += delta; });

                check(prompt_res.ok, "Multi-step prompt succeeded");
                check(prompt_res.message == "The final calculation is 84.", "Final message matches");
                check(turn_count == 3, "Expected 3 turns (prompt -> tool 1 -> tool 2 -> final text)");

                // Inspect captured payloads to prove exact protocol adherence:
                check(captured_payloads.size() == 3, "Must have captured 3 outgoing requests");

                // Request 1: Initial turn, contains user prompt
                const auto& req1_msgs = captured_payloads[0].at("messages");
                check(!req1_msgs.empty(), "Req 1 messages not empty");

                // Request 2: Must contain assistant message with tool 1 AND tool message with EXACT matching tool_call_id
                const auto& req2_msgs = captured_payloads[1].at("messages");
                check(req2_msgs.size() >= 3, "Req 2 must have at least 3 messages");

                // Find assistant tool call message
                bool found_asst1 = false;
                std::string asst1_id;
                for (const auto& m : req2_msgs) {
                    if (m.value("role", "") == "assistant" && m.contains("tool_calls")) {
                        found_asst1 = true;
                        const auto& tc = m.at("tool_calls");
                        check(tc.size() == 1, "Assistant must have 1 tool call");
                        asst1_id = tc[0].value("id", "");
                        check(asst1_id == "call_add_step1", "Reconstructed tool-call ID mismatch");
                        check(tc[0]["function"].value("name", "") == "add", "Reconstructed tool name mismatch");
                    }
                }
                check(found_asst1, "Req 2 must contain assistant message with tool_calls");

                // Find tool response message and check exact correspondence
                bool found_tool1 = false;
                for (const auto& m : req2_msgs) {
                    if (m.value("role", "") == "tool") {
                        found_tool1 = true;
                        check(m.value("tool_call_id", "") == asst1_id, "Tool message tool_call_id must match assistant tool call ID");
                        check(m.value("content", "").find("42") != std::string::npos, "Tool message content must contain result");
                    }
                }
                check(found_tool1, "Req 2 must contain role:tool message");

                // Request 3: Must contain entire history with both tool rounds preserved
                const auto& req3_msgs = captured_payloads[2].at("messages");
                check(req3_msgs.size() >= 5, "Req 3 must contain all sequential turns");

                // Verify Turn 2 tool call and response in Request 3
                bool found_asst2 = false;
                std::string asst2_id;
                bool found_tool2 = false;
                for (const auto& m : req3_msgs) {
                    if (m.value("role", "") == "assistant" && m.contains("tool_calls")) {
                        const auto& tc = m.at("tool_calls");
                        if (tc[0].value("id", "") == "call_mul_step2") {
                            found_asst2 = true;
                            asst2_id = tc[0].value("id", "");
                            check(tc[0]["function"].value("name", "") == "multiply", "Tool 2 name mismatch");
                        }
                    }
                    if (m.value("role", "") == "tool" && m.value("tool_call_id", "") == "call_mul_step2") {
                        found_tool2 = true;
                        check(m.value("content", "").find("84") != std::string::npos, "Tool 2 content must contain 84");
                    }
                }
                check(found_asst2, "Req 3 must contain assistant message for tool 2");
                check(found_tool2, "Req 3 must contain role:tool message for tool 2 with matching tool_call_id");

                // Turn 4: Follow-up user prompt to verify that Turn 3's final assistant response
                // remains in history, and the history sequence is strictly:
                // assistant (tool_calls) -> role:"tool" -> next assistant response -> user follow-up
                auto follow_up_res = session.prompt("Proceed with follow-up");
                check(follow_up_res.ok, "Follow-up turn succeeded");
                check(follow_up_res.message == "Follow-up completed successfully.", "Follow-up message matches");
                check(turn_count == 4, "Expected 4 turns total (tool 1 -> tool 2 -> final text -> follow-up)");

                check(captured_payloads.size() == 4, "Must have captured 4 outgoing requests");
                const auto& req4_msgs = captured_payloads[3].at("messages");
                check(req4_msgs.size() >= 7, "Req 4 must contain all preceding turns and follow-up");

                std::size_t asst2_pos = 0;
                std::size_t tool2_pos = 0;
                std::size_t asst_final_pos = 0;
                std::size_t user_followup_pos = 0;

                for (std::size_t i = 0; i < req4_msgs.size(); ++i) {
                    const auto& m = req4_msgs[i];
                    if (m.value("role", "") == "assistant" && m.contains("tool_calls")) {
                        if (m["tool_calls"][0].value("id", "") == "call_mul_step2") {
                            asst2_pos = i;
                        }
                    }
                    if (m.value("role", "") == "tool" && m.value("tool_call_id", "") == "call_mul_step2") {
                        tool2_pos = i;
                    }
                    if (m.value("role", "") == "assistant" && !m.contains("tool_calls") &&
                        m.value("content", "") == "The final calculation is 84.") {
                        asst_final_pos = i;
                    }
                    if (m.value("role", "") == "user" && m.value("content", "") == "Proceed with follow-up") {
                        user_followup_pos = i;
                    }
                }

                check(asst2_pos > 0, "Found assistant message with tool 2 in Req 4");
                check(tool2_pos == asst2_pos + 1, "Role:tool message immediately follows assistant tool_call");
                check(asst_final_pos == tool2_pos + 1, "Next assistant response immediately follows role:tool message");
                check(user_followup_pos == asst_final_pos + 1, "User follow-up immediately follows assistant response");

                mock_server.stop();
                if (server_thread.joinable())
                    server_thread.join();
            } catch (...) {
                mock_server.stop();
                if (server_thread.joinable())
                    server_thread.join();
                throw;
            }
        }

        // 12. Multiple tool calls in single turn distinguished by index with payload verification
        {
            std::cout << "Running multiple parallel tool calls test with payload inspection...\n";
            httplib::Server mock_server;
            int turn_count = 0;
            nlohmann::json captured_req2;
            nlohmann::json captured_req3;
            std::mutex payload_mutex;

            mock_server.Get("/v1/models", [](const httplib::Request&, httplib::Response& res) {
                res.set_content(R"({
                    "data": [
                        {
                            "id": "kiro",
                            "name": "Kiro via OmniRoute",
                            "supported_parameters": ["tools"]
                        }
                    ]
                })", "application/json");
            });

            mock_server.Post("/v1/chat/completions", [&](const httplib::Request& req, httplib::Response& res) {
                ++turn_count;
                res.set_header("Content-Type", "text/event-stream");
                res.set_header("Cache-Control", "no-cache");

                if (turn_count == 1) {
                    // Turn 1: Model requests two tool calls concurrently (index 0 and index 1)
                    std::string c1 = "data: {\"choices\": [{\"delta\": {\"tool_calls\": ["
                        "{\"index\": 0, \"id\": \"call_idx0_\", \"type\": \"function\", \"function\": {\"name\": \"add\", \"arguments\": \"{\\\"a\\\": 10,\"}},"
                        "{\"index\": 1, \"id\": \"call_idx1_\", \"type\": \"function\", \"function\": {\"name\": \"multiply\", \"arguments\": \"{\\\"a\\\": 6,\"}}"
                        "]}}]}\n\n";
                    std::string c2 = "data: {\"choices\": [{\"delta\": {\"tool_calls\": ["
                        "{\"index\": 0, \"id\": \"abc\", \"function\": {\"arguments\": \" \\\"b\\\": 20}\"}},"
                        "{\"index\": 1, \"id\": \"xyz\", \"function\": {\"arguments\": \" \\\"b\\\": 7}\"}}"
                        "]}}]}\n\n";
                    std::string c3 = "data: [DONE]\n\n";
                    res.set_content(c1 + c2 + c3, "text/event-stream");
                } else if (turn_count == 2) {
                    {
                        std::lock_guard lock(payload_mutex);
                        captured_req2 = nlohmann::json::parse(req.body);
                    }
                    std::string c1 = "data: {\"choices\": [{\"delta\": {\"content\": \"Calculated both 30 and 42.\"}}]}\n\n";
                    std::string c2 = "data: [DONE]\n\n";
                    res.set_content(c1 + c2, "text/event-stream");
                } else {
                    {
                        std::lock_guard lock(payload_mutex);
                        captured_req3 = nlohmann::json::parse(req.body);
                    }
                    std::string c1 = "data: {\"choices\": [{\"delta\": {\"content\": \"Parallel summary complete.\"}}]}\n\n";
                    std::string c2 = "data: [DONE]\n\n";
                    res.set_content(c1 + c2, "text/event-stream");
                }
            });

            const int port = mock_server.bind_to_any_port("127.0.0.1");
            check(port > 0, "Mock server failed to bind port");

            std::thread server_thread([&] { mock_server.listen_after_bind(); });

            try {
                arn::core::OmniRouteConfig cfg{
                    .endpoint = "http://127.0.0.1:" + std::to_string(port),
                    .api_path_prefix = "/v1"
                };

                auto provider = std::make_shared<arn::core::OmniRouteProvider>(cfg);
                auto tools = std::make_shared<arn::core::ToolRegistry>();
                tools->register_tool(std::make_shared<AddTool>());
                tools->register_tool(std::make_shared<MultiplyTool>());

                arn::core::AgentSession session;
                session.set_tools(tools);

                auto config_res = session.configure_provider(provider, "fake-omniroute-key");
                check(config_res.ok, "Configure OmniRoute provider");

                auto prompt_res = session.prompt("Run parallel calculations");
                check(prompt_res.ok, "Prompt succeeded");
                check(prompt_res.message == "Calculated both 30 and 42.", "Final message matches");
                check(turn_count == 2, "Turn count == 2");

                // Inspect captured outgoing Request 2 payload
                const auto& msgs = captured_req2.at("messages");

                // Verify assistant message with both tool calls
                bool found_asst = false;
                std::string id0, id1;
                for (const auto& m : msgs) {
                    if (m.value("role", "") == "assistant" && m.contains("tool_calls")) {
                        found_asst = true;
                        const auto& tcs = m.at("tool_calls");
                        check(tcs.size() == 2, "Assistant message must contain 2 tool calls");
                        id0 = tcs[0].value("id", "");
                        id1 = tcs[1].value("id", "");
                        check(id0 == "call_idx0_abc", "Tool 0 ID mismatch");
                        check(tcs[0]["function"].value("name", "") == "add", "Tool 0 name mismatch");
                        check(id1 == "call_idx1_xyz", "Tool 1 ID mismatch");
                        check(tcs[1]["function"].value("name", "") == "multiply", "Tool 1 name mismatch");
                    }
                }
                check(found_asst, "Found assistant message with 2 tool calls");

                // Verify both role: "tool" messages exist with exact matching tool_call_id
                bool found_resp0 = false;
                bool found_resp1 = false;
                for (const auto& m : msgs) {
                    if (m.value("role", "") == "tool") {
                        if (m.value("tool_call_id", "") == id0) {
                            found_resp0 = true;
                            check(m.value("content", "").find("30") != std::string::npos, "Tool 0 result");
                        }
                        if (m.value("tool_call_id", "") == id1) {
                            found_resp1 = true;
                            check(m.value("content", "").find("42") != std::string::npos, "Tool 1 result");
                        }
                    }
                }
                check(found_resp0, "Outgoing message contains role:tool for call_idx0_abc");
                check(found_resp1, "Outgoing message contains role:tool for call_idx1_xyz");

                // Turn 3: Follow-up prompt to verify that Request 3 retains:
                // assistant (2 tool calls) -> 2x role:"tool" -> next assistant response
                auto prompt_res2 = session.prompt("Summarize results");
                check(prompt_res2.ok, "Follow-up turn succeeded");
                check(prompt_res2.message == "Parallel summary complete.", "Follow-up text mismatch");
                check(turn_count == 3, "Turn count == 3");

                const auto& msgs3 = captured_req3.at("messages");
                std::size_t asst_tools_pos = 0;
                std::size_t tool_resp0_pos = 0;
                std::size_t tool_resp1_pos = 0;
                std::size_t asst_resp_pos = 0;

                for (std::size_t i = 0; i < msgs3.size(); ++i) {
                    const auto& m = msgs3[i];
                    if (m.value("role", "") == "assistant" && m.contains("tool_calls")) {
                        asst_tools_pos = i;
                    }
                    if (m.value("role", "") == "tool" && m.value("tool_call_id", "") == id0) {
                        tool_resp0_pos = i;
                    }
                    if (m.value("role", "") == "tool" && m.value("tool_call_id", "") == id1) {
                        tool_resp1_pos = i;
                    }
                    if (m.value("role", "") == "assistant" && !m.contains("tool_calls") &&
                        m.value("content", "") == "Calculated both 30 and 42.") {
                        asst_resp_pos = i;
                    }
                }
                check(asst_tools_pos > 0, "Found assistant message with 2 tool calls in Req 3");
                check(tool_resp0_pos > asst_tools_pos, "Tool resp 0 follows assistant tools");
                check(tool_resp1_pos > asst_tools_pos, "Tool resp 1 follows assistant tools");
                check(asst_resp_pos > tool_resp0_pos && asst_resp_pos > tool_resp1_pos,
                      "Next assistant response follows both tool responses");

                mock_server.stop();
                if (server_thread.joinable())
                    server_thread.join();
            } catch (...) {
                mock_server.stop();
                if (server_thread.joinable())
                    server_thread.join();
                throw;
            }
        }

        // 13. Streamed tool-call without explicit index property and non-string argument fallback
        {
            std::string text;
            nlohmann::json tool_calls = nlohmann::json::array();

            // Delta 1: no "index" field, specifies id, type, name, partial argument
            const std::string chunk1 = R"({
                "choices": [{
                    "delta": {
                        "tool_calls": [{
                            "id": "call_no_index_field",
                            "type": "function",
                            "function": {"name": "add", "arguments": "{\"a\": 5,"}
                        }]
                    }
                }]
            })";

            // Delta 2: no "index" field, empty id/name, completes argument
            const std::string chunk2 = R"({
                "choices": [{
                    "delta": {
                        "tool_calls": [{
                            "id": "",
                            "function": {"name": "", "arguments": " \"b\": 10}"}
                        }]
                    }
                }]
            })";

            // Delta 3: no "index" field, null id and null name (must not overwrite)
            const std::string chunk3 = R"({
                "choices": [{
                    "delta": {
                        "tool_calls": [{
                            "id": null,
                            "function": {"name": null}
                        }]
                    }
                }]
            })";

            arn::core::detail::parse_openrouter_stream_chunk(chunk1, text, tool_calls, nullptr);
            arn::core::detail::parse_openrouter_stream_chunk(chunk2, text, tool_calls, nullptr);
            arn::core::detail::parse_openrouter_stream_chunk(chunk3, text, tool_calls, nullptr);

            check(tool_calls.size() == 1, "Expected 1 aggregated tool call without index field");
            check(tool_calls[0].value("id", "") == "call_no_index_field", "Tool call ID preserved");
            check(tool_calls[0].value("type", "") == "function", "Tool call type preserved");
            check(tool_calls[0]["function"].value("name", "") == "add", "Function name preserved");
            check(tool_calls[0]["function"].value("arguments", "") == "{\"a\": 5, \"b\": 10}", "Arguments accumulated");
        }

        std::cout << "All OmniRoute provider tests passed successfully!\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "OmniRoute test error: " << error.what() << '\n';
        return 1;
    }
}
