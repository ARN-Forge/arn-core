#pragma once

#include <arn/core/provider/openrouter_provider.hpp>

#include <cstdlib>
#include <map>
#include <optional>

class ScopedOmniEnvironment {
    std::optional<std::string> primary_, legacy_;
public:
    static void set(const char* name, const char* value) {
#ifdef _WIN32
        _putenv_s(name, value ? value : "");
#else
        if (value) setenv(name, value, 1); else unsetenv(name);
#endif
    }
    ScopedOmniEnvironment() {
        if (const auto* value = std::getenv("ARN_OMNIROUTE_BASE_URL")) primary_ = value;
        if (const auto* value = std::getenv("OMNIROUTE_BASE_URL")) legacy_ = value;
        set("ARN_OMNIROUTE_BASE_URL", nullptr);
        set("OMNIROUTE_BASE_URL", nullptr);
    }
    ~ScopedOmniEnvironment() {
        set("ARN_OMNIROUTE_BASE_URL", primary_ ? primary_->c_str() : nullptr);
        set("OMNIROUTE_BASE_URL", legacy_ ? legacy_->c_str() : nullptr);
    }
};

// Offline errors are scoped by provider and HTTP status, never by a model allowlist.
void run_omniroute_invalid_model_tests() {
    using namespace arn::core;
    ScopedOmniEnvironment environment;
    constexpr std::string_view friendly = "Model unavailable for the current OmniRoute account/session. Use /models to select another model.";
    struct Case { int status; std::string body; bool unavailable; };
    const std::vector<Case> cases{
        {400, R"({"error":{"message":"[400]: Invalid model. Please select a different model to continue.","upstream_details":{"reason":"INVALID_MODEL_ID"}}})", true},
        {400, R"({"error":{"message":"Invalid model. Please select a different model to continue."}})", true},
        {400, R"({"error":"Invalid model."})", true},
        {400, R"({"message":"  INVALID MODEL. Please select another model."})", true},
        {400, "Invalid model. Please select a different model to continue.", true},
        {400, R"({"error":{"code":"invalid_model_id","message":"Upstream refused this selection."}})", true},
        {400, R"({"error":{"message":"Invalid model. FAKE_SECRET"}})", true},
        {400, R"({"error":{"message":"Invalid model parameters. Missing messages."}})", false},
        {400, R"({"error":{"message":"Invalid model configuration."}})", false},
        {400, R"({"error":{"message":"Invalid request: messages must be an array."}})", false},
        {400, R"({"error":{"message":"Unsupported parameter: temperature.","details":"Invalid model."}})", false},
        {400, R"({"error":{"message":42,"code":42}})", false},
        {400, "{broken JSON mentioning Invalid model", false},
        {401, R"({"error":{"message":"Invalid model."}})", false},
        {404, R"({"error":{"message":"Invalid model."}})", false},
        {422, R"({"error":{"message":"Invalid model."}})", false},
    };
    std::atomic_size_t selected_case{0};
    std::string last_model;
    httplib::Server server;
    const auto failure = [&](httplib::Response& res) {
        const auto& current = cases.at(selected_case.load());
        res.status = current.status;
        res.set_content(current.body, "application/json");
    };
    server.Get("/v1/models", [&](const httplib::Request& req, httplib::Response& res) {
        if (req.has_header("Authorization") && req.get_header_value("Authorization") == "Bearer FAKE_DISCOVERY_FAILURE") {
            failure(res);
        } else {
            // IDs listed by the catalog may still be rejected at completion time.
            res.set_content(R"({"data":[{"id":"kiro/fake-unavailable"},{"id":"kr/fake-unavailable"},{"id":"kiro/fake-future-model"}]})", "application/json");
        }
    });
    server.Post("/v1/chat/completions", [&](const httplib::Request& req, httplib::Response& res) {
        last_model = nlohmann::json::parse(req.body).at("model").get<std::string>();
        failure(res);
    });
    const int port = server.bind_to_any_port("127.0.0.1");
    check(port > 0, "Invalid-model fixture must bind");
    std::thread thread([&] { server.listen_after_bind(); });
    try {
        const std::string endpoint = "http://127.0.0.1:" + std::to_string(port);
        OmniRouteProvider provider(endpoint);
        const auto catalog = provider.list_models("FAKE_SECRET");
        check(catalog.ok && catalog.models == std::vector<std::string>{"kiro/fake-future-model", "kiro/fake-unavailable", "kr/fake-unavailable"},
              "Catalog is server-owned; preserve IDs without a hardcoded access filter");
        ToolRegistry tools;
        for (std::size_t index = 0; index < cases.size(); ++index) {
            selected_case = index;
            for (const std::string model : {"kiro/fake-unavailable", "kr/fake-unavailable"}) {
                provider.reset_session();
                const auto turn = provider.start_turn("FAKE_SECRET", model, "", "Привіт", tools);
                check(!turn.ok && turn.error_message.starts_with(friendly) == cases[index].unavailable,
                      "Only OmniRoute HTTP 400 invalid-model errors receive availability guidance");
                check(last_model == model, "Do not alias or rewrite kiro/ and kr/ model IDs");
                check(turn.error_message.find("HTTP " + std::to_string(cases[index].status)) != std::string::npos,
                      "Retain original provider HTTP diagnostic");
                check(turn.error_message.find("FAKE_SECRET") == std::string::npos, "Redact keys in the original error diagnostic");
                if (index == 0) check(turn.error_message.find("[400]: Invalid model.") != std::string::npos,
                                      "Retain original upstream error explanation");
            }
        }
        selected_case = 0;
        OpenRouterProvider other_provider(endpoint, "/v1");
        const auto other = other_provider.start_turn("FAKE_SECRET", "kiro/fake-unavailable", "", "Hello", tools);
        check(!other.ok && !other.error_message.starts_with(friendly), "Do not relabel errors from another provider");
        const auto discovery_failure = provider.list_models("FAKE_DISCOVERY_FAILURE");
        check(!discovery_failure.ok && discovery_failure.message.starts_with(friendly), "Discovery preserves the same precise HTTP error handling");
        AgentSession session;
        check(session.configure_provider(std::make_unique<OmniRouteProvider>(endpoint), "FAKE_SECRET").ok, "Configure fixture session");
        session.select_model("kr/fake-unavailable");
        const auto result = session.prompt("Привіт");
        check(!result.ok && result.message.starts_with(friendly), "AgentSession forwards user-facing guidance to normal chat");
    } catch (...) {
        server.stop(); thread.join(); throw;
    }
    server.stop(); thread.join();
}

void run_omniroute_hardening_tests() {
    using namespace arn::core;
    ScopedOmniEnvironment environment;
    check(OmniRouteProvider{}.base_url() == "http://localhost:20128/v1", "Standard local OmniRoute default");
    ScopedOmniEnvironment::set("OMNIROUTE_BASE_URL", "http://legacy.example:9090/v1");
    check(OmniRouteProvider{}.base_url() == "http://legacy.example:9090/v1", "Legacy override remains supported");
    ScopedOmniEnvironment::set("ARN_OMNIROUTE_BASE_URL", "  https://route.example:8443/api/v2/  ");
    check(OmniRouteProvider{}.base_url() == "https://route.example:8443/api/v2", "Primary override wins and is normalized");
    ScopedOmniEnvironment::set("ARN_OMNIROUTE_BASE_URL", "");
    check(OmniRouteProvider{}.base_url() == "http://legacy.example:9090/v1", "Empty primary override permits legacy value");
    ScopedOmniEnvironment::set("OMNIROUTE_BASE_URL", nullptr);
    check(OmniRouteProvider{}.base_url() == "http://localhost:20128/v1", "Empty override keeps standard default");
    for (const char* bad : {" ", "ftp://localhost/v1", "http:///v1", "http://localhost:invalid/v1",
                           "http://localhost:0/v1", "http://localhost:65536/v1", "http://localhost:/v1",
                           "http://fake-user:FAKE_SECRET@localhost/v1", "http://localhost/v1?key=FAKE_SECRET",
                           "http://localhost/v1#fragment", "http://local host/v1", "http://localhost/v1\nextra"}) {
        ScopedOmniEnvironment::set("ARN_OMNIROUTE_BASE_URL", bad);
        OmniRouteProvider provider;
        check(provider.base_url().empty(), "Malformed override is not exposed or silently replaced");
        const auto result = provider.list_models("FAKE_SECRET");
        check(!result.ok && result.message.find("endpoint") != std::string::npos, "Malformed endpoint fails before network");
        check(result.message.find("FAKE_SECRET") == std::string::npos, "Endpoint error must not echo credentials");
    }
    ScopedOmniEnvironment::set("ARN_OMNIROUTE_BASE_URL", nullptr);
    const auto [endpoint, prefix] = parse_base_url("localhost:20128/custom/v1/");
    check(endpoint == "http://localhost:20128" && prefix == "/custom/v1", "Scheme-less custom endpoint compatibility");
    check(parse_base_url("http://[::1]:20128/v1").first == "http://[::1]:20128", "IPv6 custom endpoint");

    OmniRouteProvider root_api(OmniRouteConfig{.endpoint = "http://localhost:9999", .api_path_prefix = ""});
    check(root_api.base_url() == "http://localhost:9999", "Programmatic empty/root API prefix is preserved");
    OmniRouteProvider recovered;
    recovered.set_base_url("ftp://localhost/v1");
    check(!recovered.list_models("FAKE_SECRET").ok, "Invalid runtime override must fail");
    recovered.set_base_url("http://localhost:20128/v1");
    check(recovered.base_url() == "http://localhost:20128/v1", "Correcting the URL clears configuration failure");

    // Full snapshots after streamed tool fragments must not duplicate IDs or arguments.
    {
        std::string output;
        nlohmann::json calls = nlohmann::json::array();
        const auto parse = [&](std::string_view event) {
            detail::parse_openrouter_stream_chunk(event, output, calls, {});
        };
        parse(R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"fake_","function":{"name":"add","arguments":"{\"a\":"}}]}}]})");
        parse(R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call","function":{"arguments":"1,\"b\":2}"}}]}}]})");
        parse(R"({"choices":[{"delta":{},"message":{"content":null,"tool_calls":[{"id":"fake_call","function":{"name":"add","arguments":"{\"a\":1,\"b\":2}"}}]}}]})");
        check(calls.size() == 1 && calls[0]["id"] == "fake_call" && calls[0]["function"]["name"] == "add" &&
              calls[0]["function"]["arguments"] == R"({"a":1,"b":2})", "Full tool snapshot must not concatenate completed values twice");
    }

    // Captured OmniRoute 3.8.50 Claude SSE shape: role, text deltas, finish/usage, DONE.
    // Response text is replaced with an obvious fixture, retaining the actual wire structure.
    const std::string observed_sse =
        "data: {\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\"},\"finish_reason\":null}]}\n\n"
        "data: {\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Привіт\"},\"finish_reason\":null}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"!\"},\"finish_reason\":null}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}],\"usage\":{\"completion_tokens\":2}}\n\n"
        "data: [DONE]\n\n";
    // This is the real error shape observed for kiro/gpt-5.6-sol, not a fabricated model delta.
    const std::string observed_error = R"({"error":{"message":"[400]: Invalid model. Please select a different model to continue.","type":"upstream_error","upstream_details":{"reason":"INVALID_MODEL_ID"}}})";
    struct Reply { int status; std::string type, body, text; bool ok; };
    const std::map<std::string, Reply> replies{
        {"observed-sse", {200, "text/event-stream", observed_sse, "Привіт!", true}},
        {"observed-invalid-model", {400, "application/json", observed_error, "", false}},
        // Additional OpenAI-compatible protocol variants are contract tests; they were
        // not observed from the currently rejected gpt model.
        {"content-parts", {200, "text/event-stream", "data: {\"choices\":[{\"delta\":{\"content\":[{\"type\":\"text\",\"text\":\"Visible\"},{\"type\":\"reasoning\",\"text\":\"PRIVATE_REASONING\"}]}}]}\n\ndata: [DONE]\n\n", "Visible", true}},
        {"message-snapshot", {200, "text/event-stream", "data: {\"choices\":[{\"delta\":{\"content\":\"Hi\"}}]}\n\ndata: {\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"Hi there\"}}]}\n\ndata: [DONE]\n\n", "Hi there", true}},
        {"json-completion", {200, "application/json; charset=utf-8", R"({"choices":[{"message":{"role":"assistant","content":"Привіт!"}}]})", "Привіт!", true}},
        {"legacy-text", {200, "text/event-stream", "data: {\"choices\":[{\"text\":\"Hello\"}]}\n\ndata: [DONE]\n\n", "Hello", true}},
        {"json-error", {200, "application/json", observed_error, "", false}},
        {"sse-error", {200, "text/event-stream", "data: " + observed_error + "\n\ndata: [DONE]\n\n", "", false}},
        {"partial-error", {200, "text/event-stream", "data: {\"choices\":[{\"delta\":{\"content\":\"Partial\"}}]}\n\ndata: " + observed_error + "\n\n", "", false}},
        {"echoed-secret-error", {200, "application/json", R"({"error":{"message":"Rejected FAKE_SECRET"}})", "", false}},
        {"upstream-nonstring", {200, "application/json", R"({"error":{"message":42}})", "", false}},
        {"http-echoed-secret", {400, "application/json", R"({"error":{"message":"Rejected FAKE_SECRET"}})", "", false}},
        {"unknown-data", {200, "text/event-stream", "data: {\"unrecognized\":\"FAKE_RESPONSE\"}\n\ndata: [DONE]\n\n", "", false}},
        {"malformed-data", {200, "text/event-stream", "data: {bad json}\n\ndata: [DONE]\n\n", "", false}},
        {"empty-stream", {200, "text/event-stream", "data: [DONE]\n\n", "", false}},
        {"role-only", {200, "text/event-stream", "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\"},\"finish_reason\":\"stop\"}]}\n\n", "", false}},
        {"reasoning-only", {200, "text/event-stream", "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"PRIVATE_REASONING\"}}]}\n\n", "", false}},
        {"tool-with-malformed-content", {200, "text/event-stream", "data: {\"choices\":[{\"delta\":{\"content\":42,\"tool_calls\":[{\"index\":0,\"id\":\"fake_call\",\"function\":{\"name\":\"add\",\"arguments\":\"{\\\"a\\\":1,\\\"b\\\":2}\"}}]}}]}\n\ndata: [DONE]\n\n", "", true}},
    };
    httplib::Server server;
    std::atomic_int discoveries{0};
    server.Get("/custom/v1/models", [&](const httplib::Request& req, httplib::Response& res) {
        check(req.get_header_value("Authorization") == "Bearer FAKE_SECRET", "Key validates against effective endpoint");
        ++discoveries;
        res.set_content(R"({"data":[{"id":"observed-sse"}]})", "application/json");
    });
    server.Post("/custom/v1/chat/completions", [&](const httplib::Request& req, httplib::Response& res) {
        const auto reply = replies.at(nlohmann::json::parse(req.body).at("model").get<std::string>());
        res.status = reply.status;
        res.set_content(reply.body, reply.type);
    });
    const int port = server.bind_to_any_port("127.0.0.1");
    check(port > 0, "Hardening fixture server must bind");
    std::thread thread([&] { server.listen_after_bind(); });
    try {
        const auto url = "http://127.0.0.1:" + std::to_string(port) + "/custom/v1";
        ScopedOmniEnvironment::set("ARN_OMNIROUTE_BASE_URL", url.c_str());
        OmniRouteProvider provider;
        check(provider.base_url() == url, "Actual provider endpoint reflects custom API path");
        check(provider.list_models("FAKE_SECRET").ok && discoveries == 1, "Key discovery uses resolved endpoint");
        ToolRegistry tools;
        for (const auto& [model, reply] : replies) {
            provider.reset_session();
            std::string rendered;
            StreamCallbacks callbacks;
            callbacks.on_text = [&](std::string_view text) { rendered += text; };
            const auto turn = provider.start_turn("FAKE_SECRET", model, "", "Привіт", tools, callbacks);
            check(turn.ok == reply.ok, "Unexpected protocol outcome");
            if (turn.ok) {
                check(turn.text == reply.text && rendered == reply.text, "Text must reach both final turn and streaming callback exactly once");
                if (model == "tool-with-malformed-content")
                    check(turn.tool_calls.size() == 1 && turn.tool_calls[0].id == "fake_call" && turn.tool_calls[0].arguments.at("a") == 1,
                          "Unexpected content must not discard valid tool calls");
            } else {
                check(!turn.error_message.empty(), "Unsupported/empty responses must explain failure");
                check(turn.error_message.find("FAKE_SECRET") == std::string::npos, "Errors must not echo the request key");
                check(provider.session_entries() == 1, "Failed response must not append an empty assistant message");
                if (model.find("error") != std::string::npos || model == "observed-invalid-model")
                    check(turn.error_message.find("Invalid model") != std::string::npos || model == "echoed-secret-error", "Preserve upstream error explanation");
            }
            check(rendered.find("PRIVATE_REASONING") == std::string::npos, "Reasoning is not an assistant answer");
        }
        std::atomic_bool cancelled{false};
        StreamCallbacks callbacks;
        callbacks.on_text = [&](std::string_view) { cancelled = true; };
        const auto turn = provider.start_turn("FAKE_SECRET", "observed-sse", "", "Привіт", tools, callbacks, &cancelled);
        check(!turn.ok && turn.cancelled, "Cancellation during streamed text is preserved");

        AgentSession session;
        check(session.configure_provider(std::make_unique<OmniRouteProvider>(), "FAKE_SECRET").ok, "AgentSession resolves endpoint");
        session.select_model("observed-sse");
        const auto result = session.prompt("Привіт");
        check(result.ok && result.message == "Привіт!", "AgentSession receives visible streamed result");
    } catch (...) {
        server.stop(); thread.join(); throw;
    }
    server.stop(); thread.join();
    run_omniroute_invalid_model_tests();
}
