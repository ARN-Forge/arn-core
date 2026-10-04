#include "arn/core/provider/openai_provider.hpp"
#include "arn/core/net/http_client.hpp"
#include "arn/core/net/sse_decoder.hpp"
#include "provider_utils.hpp"

#include <httplib.h>

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <charconv>
#include <ranges>
#include <utility>

namespace arn::core {

namespace {
std::string redact_key(std::string message, std::string_view key) {
    if (key.empty()) return message;
    std::size_t pos = 0;
    while ((pos = message.find(key, pos)) != std::string::npos) {
        message.replace(pos, key.size(), "[redacted]");
        pos += 10;
    }
    return message;
}

bool invalid_model_message(std::string_view message) {
    const auto lower = detail::lower_ascii(std::string(message));
    message = lower;
    const auto trim = [&] {
        const auto start = message.find_first_not_of(" \t\r\n");
        message = start == std::string_view::npos ? std::string_view{} : message.substr(start);
    };
    trim();
    // OmniRoute prefixes its upstream error message with the HTTP status.
    if (message.starts_with("[400]")) {
        message.remove_prefix(5);
        trim();
        if (message.starts_with(':')) message.remove_prefix(1);
        trim();
    }
    constexpr std::string_view marker = "invalid model";
    if (!message.starts_with(marker)) return false;
    message.remove_prefix(marker.size());
    trim();
    // Do not misclassify "Invalid model parameters/configuration" as lack of access.
    return message.empty() || message.front() == '.' || message.front() == ':' ||
           message.front() == '!' || message.front() == ';';
}

bool invalid_model_response(std::string_view body) {
    const auto parsed = nlohmann::json::parse(body, nullptr, false);
    if (parsed.is_discarded()) return invalid_model_message(body);
    if (!parsed.is_object()) return false;
    const auto error_it = parsed.find("error");
    const auto& error = error_it == parsed.end() ? parsed : *error_it;
    if (error.is_string()) return invalid_model_message(error.get_ref<const std::string&>());
    if (!error.is_object()) return false;
    const auto invalid_code = [](const nlohmann::json& object, const char* field) {
        const auto value = object.find(field);
        if (value == object.end() || !value->is_string()) return false;
        const auto code = detail::lower_ascii(value->get<std::string>());
        return code == "invalid_model" || code == "invalid_model_id";
    };
    if (invalid_code(error, "code") || invalid_code(error, "reason")) return true;
    const auto details = error.find("upstream_details");
    if (details != error.end() && details->is_object() && invalid_code(*details, "reason")) return true;
    const auto message = error.find("message");
    return message != error.end() && message->is_string() &&
           invalid_model_message(message->get_ref<const std::string&>());
}

ApiResult provider_http_error(int status, const std::string& body,
                              const std::string& provider_name, ProviderType provider_type,
                              std::string_view api_key) {
    auto error = detail::parse_error(status, provider_name, body);
    if (provider_type == ProviderType::omniroute && status == 400 && invalid_model_response(body)) {
        // Catalog discovery is not an entitlement check. Treat model IDs as opaque:
        // kiro/ and kr/ are independent namespaces, with no local routing/allowlist.
        error.message = "Model unavailable for the current OmniRoute account/session. "
                        "Use /models to select another model.\n" + error.message;
    }
    error.message = redact_key(std::move(error.message), api_key);
    return error;
}

} // namespace

std::pair<std::string, std::string> parse_base_url(std::string_view url,
                                                  std::string_view default_prefix) {
    while (!url.empty() && (url.front() == ' ' || url.front() == '\t' || url.front() == '\r' || url.front() == '\n')) {
        url.remove_prefix(1);
    }
    while (!url.empty() && (url.back() == ' ' || url.back() == '\t' || url.back() == '\r' || url.back() == '\n')) {
        url.remove_suffix(1);
    }
    const auto invalid = [] { throw std::invalid_argument("Invalid API endpoint: use an HTTP(S) URL with a host and optional port/path, without credentials, query or fragment."); };
    if (url.empty() || std::ranges::any_of(url, [](unsigned char c) { return c <= 32 || c == 127 || c == '\\'; }) ||
        url.find_first_of("@?#") != std::string_view::npos)
        invalid();

    std::string_view scheme;
    std::string_view rest = url;
    const auto scheme_end = url.find("://");
    if (scheme_end != std::string_view::npos) {
        scheme = url.substr(0, scheme_end + 3);
        rest = url.substr(scheme_end + 3);
    } else {
        scheme = "http://";
    }

    if (scheme != "http://" && scheme != "https://") invalid();
    const auto path_start = rest.find('/');
    const auto authority = rest.substr(0, path_start);
    if (authority.empty()) invalid();
    std::string_view host = authority, port;
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos || close <= 1) invalid();
        host = authority.substr(1, close - 1);
        if (host.find(':') == std::string_view::npos ||
            host.find_first_not_of("0123456789abcdefABCDEF:.") != std::string_view::npos) invalid();
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') invalid();
            port = authority.substr(close + 2);
            if (port.empty()) invalid();
        }
    } else {
        const auto colon = authority.find(':');
        host = authority.substr(0, colon);
        if (host.empty() || host.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_") != std::string_view::npos) invalid();
        if (colon != std::string_view::npos) {
            port = authority.substr(colon + 1);
            if (port.empty()) invalid();
        }
    }
    if (!port.empty()) {
        unsigned value = 0;
        const auto result = std::from_chars(port.data(), port.data() + port.size(), value);
        if (result.ec != std::errc{} || result.ptr != port.data() + port.size() || value == 0 || value > 65535) invalid();
    }
    if (!default_prefix.empty() && (!default_prefix.starts_with('/') ||
        default_prefix.find_first_of("@?#\\ \t\r\n") != std::string_view::npos)) invalid();
    std::string endpoint;
    std::string prefix;
    if (path_start == std::string_view::npos) {
        endpoint = std::string(scheme) + std::string(rest);
        prefix = std::string(default_prefix);
    } else {
        endpoint = std::string(scheme) + std::string(rest.substr(0, path_start));
        std::string_view path = rest.substr(path_start);
        while (path.ends_with('/') && path.size() > 1) {
            path.remove_suffix(1);
        }
        if (path.empty() || path == "/") {
            prefix = std::string(default_prefix);
        } else {
            prefix = std::string(path);
        }
    }
    return {endpoint, prefix};
}

OpenAiCompatibleProvider::OpenAiCompatibleProvider(OpenAiProviderConfig config)
    : config_(std::move(config)) {
    init_client();
}

OpenAiCompatibleProvider::~OpenAiCompatibleProvider() = default;

OpenAiCompatibleProvider::OpenAiCompatibleProvider(OpenAiCompatibleProvider&&) noexcept = default;
OpenAiCompatibleProvider& OpenAiCompatibleProvider::operator=(OpenAiCompatibleProvider&&) noexcept = default;

void OpenAiCompatibleProvider::init_client() {
    client_.reset();
    configuration_error_.clear();
    try {
        auto [endpoint, prefix] = parse_base_url(config_.endpoint, config_.api_path_prefix);
        config_.endpoint = std::move(endpoint);
        config_.api_path_prefix = std::move(prefix);
        client_ = std::make_unique<httplib::Client>(config_.endpoint);
        if (!client_->is_valid()) throw std::invalid_argument("Invalid API endpoint.");
        client_->set_connection_timeout(10, 0);
        client_->set_read_timeout(90, 0);
        client_->set_keep_alive(true);
    } catch (const std::exception&) {
        client_.reset();
        configuration_error_ = "Invalid API endpoint configuration. Use an HTTP(S) URL without credentials, query or fragment.";
    }
}

void OpenAiCompatibleProvider::set_provider_config(OpenAiProviderConfig config) {
    config_ = std::move(config);
    init_client();
}

void OpenAiCompatibleProvider::set_base_url(std::string_view url) {
    config_.endpoint = url;
    config_.api_path_prefix = "/v1";
    init_client();
}

std::string OpenAiCompatibleProvider::base_url() const {
    if (!configuration_error_.empty()) return {};
    return detail::join_api_path(config_.endpoint, config_.api_path_prefix);
}

ModelCapabilities OpenAiCompatibleProvider::model_capabilities(std::string_view model) const {
    const auto it = model_capabilities_.find(model);
    if (it != model_capabilities_.end())
        return it->second;
    return ModelCapabilities{};
}

ApiResult OpenAiCompatibleProvider::list_models(const std::string& api_key,
                                                const std::atomic_bool* cancel_requested) {
    if (cancel_requested && cancel_requested->load())
        return {false, "Request cancelled.", {}, true};
    if (!client_)
        return {false, configuration_error_.empty() ? "Client not initialized." : configuration_error_};
    client_->set_connection_timeout(10, 0);
    client_->set_read_timeout(30, 0);

    httplib::Headers headers = {
        {"Authorization", "Bearer " + api_key}
    };
    for (const auto& [header_name, header_value] : config_.custom_headers) {
        if (!header_value.empty()) {
            headers.emplace(header_name, header_value);
        }
    }

    const std::string models_path = detail::join_api_path(config_.api_path_prefix, "/models");
    const auto response = net::execute_with_retry(
        [&] { return client_->Get(models_path.c_str(), headers); },
        cancel_requested);
    if (cancel_requested && cancel_requested->load())
        return {false, "Request cancelled.", {}, true};
    if (!response)
        return {false, "Network request failed: " + httplib::to_string(response.error())};
    if (response->status < 200 || response->status >= 300) {
        return provider_http_error(response->status, response->body, config_.provider_name, type(), api_key);
    }
    try {
        auto entries = detail::parse_openrouter_model_catalog(response->body);
        std::vector<std::string> models;
        models.reserve(entries.size());
        model_capabilities_.clear();
        for (auto& entry : entries) {
            model_capabilities_.emplace(entry.id, entry.capabilities);
            models.push_back(std::move(entry.id));
        }
        return {true, config_.provider_name + " key verified.", std::move(models)};
    } catch (const std::exception& exception) {
        return {false, "Could not read the model list: " + std::string(exception.what())};
    }
}

void OpenAiCompatibleProvider::trim_history(std::size_t max_entries) {
    const std::size_t keep_from = (!messages_.empty() && messages_.front().value("role", "") == "system") ? 1 : 0;
    while (messages_.size() > max_entries) {
        messages_.erase(messages_.begin() + static_cast<nlohmann::json::difference_type>(keep_from));
    }
}

ModelTurn OpenAiCompatibleProvider::start_turn(
    const std::string& api_key, const std::string& model,
    const std::string& system_instruction, const std::string& user_prompt,
    const ToolRegistry& tools,
    const StreamCallbacks& callbacks,
    const std::atomic_bool* cancel_requested) {

    if (cancel_requested && cancel_requested->load())
        return {.ok = false, .error_message = "Request cancelled.", .cancelled = true};

    const auto caps = model_capabilities(model);
    if (messages_.empty() && !system_instruction.empty()) {
        if (caps.supports_system_instruction) {
            messages_.push_back({{"role", "system"}, {"content", system_instruction}});
        }
    }
    std::string prompt_text = user_prompt;
    if (!caps.supports_system_instruction && !system_instruction.empty() && messages_.empty()) {
        prompt_text = system_instruction + "\n\n" + user_prompt;
    }
    messages_.push_back({{"role", "user"}, {"content", std::move(prompt_text)}});
    return execute_turn_request(api_key, model, tools, callbacks, cancel_requested);
}

ModelTurn OpenAiCompatibleProvider::continue_turn(
    const std::string& api_key, const std::string& model,
    const std::string& /*system_instruction*/,
    const std::vector<ToolResponse>& tool_responses,
    const ToolRegistry& tools,
    const StreamCallbacks& callbacks,
    const std::atomic_bool* cancel_requested) {

    if (cancel_requested && cancel_requested->load())
        return {.ok = false, .error_message = "Request cancelled.", .cancelled = true};

    for (const auto& resp : tool_responses) {
        std::string content_str = resp.result.is_string() ? resp.result.get<std::string>() : resp.result.dump();
        messages_.push_back({
            {"role", "tool"},
            {"tool_call_id", resp.call_id},
            {"content", std::move(content_str)}
        });
    }
    return execute_turn_request(api_key, model, tools, callbacks, cancel_requested);
}

ModelTurn OpenAiCompatibleProvider::execute_turn_request(
    const std::string& api_key, const std::string& model,
    const ToolRegistry& tools,
    const StreamCallbacks& callbacks,
    const std::atomic_bool* cancel_requested) {

    if (!client_)
        return {.ok = false, .error_message = configuration_error_.empty() ? "Client not initialized." : configuration_error_};
    if (cancel_requested && cancel_requested->load())
        return {.ok = false, .error_message = "Request cancelled.", .cancelled = true};

    client_->set_read_timeout(90, 0);

    const auto caps = model_capabilities(model);
    const auto payload = detail::build_openrouter_payload(model, messages_, tools, caps.supports_tools);

    std::string text;
    nlohmann::json calls = nlohmann::json::array();
    std::string error_body;
    bool received_event = false;
    std::string stream_error;

    httplib::Headers headers = {
        {"Authorization", "Bearer " + api_key}
    };
    for (const auto& [header_name, header_value] : config_.custom_headers) {
        if (!header_value.empty()) {
            headers.emplace(header_name, header_value);
        }
    }

    const std::string chat_path = detail::join_api_path(config_.api_path_prefix, "/chat/completions");

    const auto response = net::execute_stream_with_retry(
        [&] {
            return net::stream_post(
                *client_, chat_path.c_str(), headers,
                payload.dump(),
                [&](std::string_view event) {
                    detail::parse_openrouter_stream_chunk(event, text, calls, callbacks.on_text, &stream_error);
                },
                error_body, received_event, cancel_requested);
        },
        received_event, error_body, cancel_requested, callbacks.on_progress);

    if (cancel_requested && cancel_requested->load(std::memory_order_relaxed)) {
        return {.ok = false, .error_message = "Request cancelled.", .cancelled = true};
    }
    if (!response) {
        return {.ok = false, .error_message = "Network request failed: " + httplib::to_string(response.error())};
    }
    if (response->status < 200 || response->status >= 300) {
        const auto error = provider_http_error(response->status, error_body, config_.provider_name, type(), api_key);
        return {.ok = false, .error_message = error.message};
    }

    if (!stream_error.empty()) {
        // Upstream errors sometimes echo credentials; never propagate this request's key.
        stream_error = redact_key(std::move(stream_error), api_key);
        return {.ok = false, .error_message = config_.provider_name + " stream error: " + stream_error};
    }

    try {
        nlohmann::json valid_calls = nlohmann::json::array();
        std::vector<ToolCall> tool_calls;
        for (const auto& call : calls) {
            if (!call.is_object())
                continue;
            std::string call_id = call.value("id", "");
            std::string call_name;
            if (call.contains("function") && call["function"].is_object()) {
                call_name = call["function"].value("name", "");
            }
            if (call_name.empty()) {
                continue;
            }
            std::string args_str;
            if (call.contains("function") && call["function"].is_object()) {
                if (call["function"].contains("arguments")) {
                    if (call["function"]["arguments"].is_string()) {
                        args_str = call["function"]["arguments"].get<std::string>();
                    } else if (!call["function"]["arguments"].is_null()) {
                        args_str = call["function"]["arguments"].dump();
                    }
                }
            }

            nlohmann::json arguments;
            try {
                arguments = nlohmann::json::parse(args_str);
            } catch (const std::exception&) {
                arguments = nlohmann::json::object();
            }

            valid_calls.push_back({
                {"id", call_id},
                {"type", call.value("type", "function")},
                {"function", {
                    {"name", call_name},
                    {"arguments", args_str}
                }}
            });

            tool_calls.push_back(ToolCall{
                .id = std::move(call_id),
                .name = std::move(call_name),
                .arguments = std::move(arguments)
            });
        }

        if (text.empty() && tool_calls.empty()) {
            return {.ok = false, .error_message = config_.provider_name +
                " returned no supported assistant text or tool calls. The upstream response was empty or used an unsupported format."};
        }
        nlohmann::json message = {
            {"role", "assistant"},
            {"content", text.empty() ? nlohmann::json(nullptr) : nlohmann::json(text)}
        };
        if (!valid_calls.empty())
            message["tool_calls"] = std::move(valid_calls);
        messages_.push_back(message);

        return {
            .ok = true,
            .text = std::move(text),
            .tool_calls = std::move(tool_calls)
        };
    } catch (const std::exception& exception) {
        return {.ok = false, .error_message = "Could not read the API response: " + std::string(exception.what())};
    }
}

void OpenAiCompatibleProvider::cancel_active_request() {
    if (client_)
        client_->stop();
}

void OpenAiCompatibleProvider::reset_session() {
    messages_ = nlohmann::json::array();
}

std::size_t OpenAiCompatibleProvider::session_entries() const noexcept {
    return messages_.size();
}

} // namespace arn::core
