# Writing a Custom Provider

This guide explains how to implement a custom model provider in **ARN Core** (`arn::core`), allowing you to connect local inference runtimes (such as Ollama, llama.cpp, vLLM), mock engines for offline testing, or internal enterprise LLM gateways.

---

## 1. When to Implement a Custom Provider

ARN Core includes built-in providers for Google Gemini, DeepSeek, and OpenRouter. You should implement a custom provider when:
- **Local Inference**: You run local models using Ollama (`http://localhost:11434`), llama.cpp server, or vLLM.
- **Enterprise Gateways**: Your organization routes LLM requests through a custom proxy with bespoke headers, encryption, or auditing.
- **Deterministic Mocking**: You want fast, fully offline automated tests without external network dependencies.

---

## 2. The `IModelProvider` Interface

All providers implement `arn::core::IModelProvider`:

```cpp
#include <arn/core/provider/model_provider.hpp>

class MyCustomProvider final : public arn::core::IModelProvider {
public:
    arn::core::ProviderType type() const noexcept override {
        return arn::core::ProviderType::custom;
    }

    std::string_view name() const noexcept override {
        return "MyProvider";
    }

    std::string preferred_model(const std::vector<std::string>& models) const override {
        return models.empty() ? "default-model" : models.front();
    }

    arn::core::ModelCapabilities model_capabilities(std::string_view model) const override {
        return arn::core::ModelCapabilities{
            .supports_tools = true,
            .supports_streaming = true,
            .supports_system_instruction = true
        };
    }

    arn::core::ApiResult list_models(const std::string& api_key,
                                     const std::atomic_bool* cancel_requested) override;

    arn::core::ModelTurn start_turn(
        const std::string& api_key, const std::string& model,
        const std::string& system_instruction, const std::string& user_prompt,
        const arn::core::ToolRegistry& tools,
        const arn::core::StreamCallbacks& callbacks,
        const std::atomic_bool* cancel_requested) override;

    arn::core::ModelTurn continue_turn(
        const std::string& api_key, const std::string& model,
        const std::string& system_instruction,
        const std::vector<arn::core::ToolResponse>& tool_responses,
        const arn::core::ToolRegistry& tools,
        const arn::core::StreamCallbacks& callbacks,
        const std::atomic_bool* cancel_requested) override;

    void trim_history(std::size_t max_entries) override;
    void cancel_active_request() override;
    void reset_session() override;
    std::size_t session_entries() const noexcept override;
};
```

---

## 3. Reference Implementation: Local Ollama Provider

Below is a complete, working example implementing a custom provider targeting a local **Ollama** server (`/api/chat`).

```cpp
#include <atomic>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <arn/core/net/http_client.hpp>
#include <arn/core/provider/model_provider.hpp>

class OllamaProvider final : public arn::core::IModelProvider {
public:
    explicit OllamaProvider(std::string host = "localhost", int port = 11434)
        : host_(std::move(host)), port_(port) {
        client_ = std::make_unique<httplib::Client>(host_, port_);
        client_->set_connection_timeout(std::chrono::seconds(5));
        client_->set_read_timeout(std::chrono::seconds(120));
    }

    arn::core::ProviderType type() const noexcept override {
        return arn::core::ProviderType::custom;
    }

    std::string_view name() const noexcept override {
        return "Ollama";
    }

    std::string preferred_model(const std::vector<std::string>& models) const override {
        for (const auto& m : models) {
            if (m.find("llama3") != std::string::npos || m.find("mistral") != std::string::npos)
                return m;
        }
        return models.empty() ? "llama3:latest" : models.front();
    }

    arn::core::ApiResult list_models(const std::string& /*api_key*/,
                                     const std::atomic_bool* cancel) override {
        auto res = client_->Get("/api/tags");
        if (!res || res->status != 200) {
            return {false, "Failed to connect to Ollama at " + host_ + ":" + std::to_string(port_)};
        }

        try {
            auto json = nlohmann::json::parse(res->body);
            std::vector<std::string> models;
            for (const auto& item : json["models"]) {
                models.push_back(item["name"].get<std::string>());
            }
            return {true, "OK", std::move(models)};
        } catch (const std::exception& e) {
            return {false, std::string("Failed to parse Ollama models: ") + e.what()};
        }
    }

    arn::core::ModelTurn start_turn(
        const std::string& /*api_key*/, const std::string& model,
        const std::string& system_instruction, const std::string& user_prompt,
        const arn::core::ToolRegistry& tools,
        const arn::core::StreamCallbacks& callbacks,
        const std::atomic_bool* cancel) override {

        // Append system message on turn 1 if not present
        if (messages_.empty() && !system_instruction.empty()) {
            messages_.push_back({{"role", "system"}, {"content", system_instruction}});
        }

        // Record user turn
        messages_.push_back({{"role", "user"}, {"content", user_prompt}});

        return execute_chat(model, tools, callbacks, cancel);
    }

    arn::core::ModelTurn continue_turn(
        const std::string& /*api_key*/, const std::string& model,
        const std::string& /*system_instruction*/,
        const std::vector<arn::core::ToolResponse>& tool_responses,
        const arn::core::ToolRegistry& tools,
        const arn::core::StreamCallbacks& callbacks,
        const std::atomic_bool* cancel) override {

        // Feed tool responses back into message history
        for (const auto& resp : tool_responses) {
            messages_.push_back({
                {"role", "tool"},
                {"content", resp.result.dump()}
            });
        }

        return execute_chat(model, tools, callbacks, cancel);
    }

    void trim_history(std::size_t max_entries) override {
        if (messages_.size() > max_entries) {
            // Keep system prompt if present
            nlohmann::json preserved = nlohmann::json::array();
            if (!messages_.empty() && messages_[0]["role"] == "system") {
                preserved.push_back(messages_[0]);
            }
            const std::size_t start = messages_.size() - (max_entries - preserved.size());
            for (std::size_t i = start; i < messages_.size(); ++i) {
                preserved.push_back(messages_[i]);
            }
            messages_ = std::move(preserved);
        }
    }

    void cancel_active_request() override {
        if (client_) {
            client_->stop();
        }
    }

    void reset_session() override {
        messages_.clear();
    }

    std::size_t session_entries() const noexcept override {
        return messages_.size();
    }

private:
    arn::core::ModelTurn execute_chat(const std::string& model,
                                      const arn::core::ToolRegistry& tools,
                                      const arn::core::StreamCallbacks& callbacks,
                                      const std::atomic_bool* cancel) {
        if (cancel && cancel->load()) {
            return {.ok = false, .error_message = "Cancelled.", .cancelled = true};
        }

        nlohmann::json payload{
            {"model", model},
            {"messages", messages_},
            {"stream", true}
        };

        // If tools are registered, attach tool declarations
        if (!tools.empty()) {
            payload["tools"] = tools.definitions_json();
        }

        std::string full_response_text;
        std::vector<arn::core::ToolCall> tool_calls;

        // Perform streaming POST request
        const std::string body = payload.dump();
        auto res = client_->Post("/api/chat", body, "application/json",
            [&](const char* data, std::size_t len) {
                if (cancel && cancel->load()) return false;

                try {
                    auto chunk = nlohmann::json::parse(std::string_view(data, len));
                    if (chunk.contains("message") && chunk["message"].contains("content")) {
                        std::string delta = chunk["message"]["content"].get<std::string>();
                        full_response_text += delta;
                        if (callbacks.on_text && !delta.empty()) {
                            callbacks.on_text(delta);
                        }
                    }
                } catch (...) {
                    // Incomplete JSON chunk across packet boundary; ignore parse failure
                }
                return true;
            });

        if (!res || res->status != 200) {
            return {.ok = false, .error_message = "Ollama request failed."};
        }

        // Record assistant turn in conversation history
        messages_.push_back({{"role", "assistant"}, {"content", full_response_text}});

        return {
            .ok = true,
            .text = full_response_text,
            .tool_calls = std::move(tool_calls)
        };
    }

    std::string host_;
    int port_;
    std::unique_ptr<httplib::Client> client_;
    nlohmann::json messages_ = nlohmann::json::array();
};
```

---

## 4. Consuming the Custom Provider

You can attach your custom provider to `AgentSession`:

```cpp
#include <arn/core/agent/agent_session.hpp>

int main() {
    auto ollama = std::make_shared<OllamaProvider>("localhost", 11434);

    arn::core::AgentSession session;
    auto setup = session.configure_provider(ollama, /*api_key=*/"");
    if (!setup.ok) {
        std::cerr << "Ollama connection error: " << setup.message << '\n';
        return 1;
    }

    std::cout << "Using Ollama model: " << session.active_model() << "\n\n";

    session.prompt("Explain the concept of zero-cost abstractions in C++.",
                   [](std::string_view delta) { std::cout << delta << std::flush; });

    return 0;
}
```
