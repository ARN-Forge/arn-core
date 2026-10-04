#include "provider_utils.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace arn::core::detail {

std::string error_message(const nlohmann::json& body) {
    if (const auto error = body.find("error"); error != body.end()) {
        if (error->is_string())
            return error->get<std::string>();
        if (error->is_object())
            return error->value("message", error->dump());
    }
    return body.value("message", "Unknown API error");
}

ApiResult parse_error(int status, const std::string& provider, const std::string& response_body) {
    try {
        return {false, provider + " returned HTTP " + std::to_string(status) + ": " +
                           error_message(nlohmann::json::parse(response_body))};
    } catch (const std::exception&) {
        return {false, provider + " returned HTTP " + std::to_string(status)};
    }
}

void trim_history(nlohmann::json& history, std::size_t keep_from) {
    while (history.size() > max_history_entries) {
        history.erase(history.begin() + static_cast<nlohmann::json::difference_type>(keep_from));
    }
}

std::string lower_ascii(std::string value) {
    std::ranges::transform(value, value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

namespace {

// Gemini function declarations accept only a subset of OpenAPI Schema. In
// particular, the v1beta function-declaration endpoint rejects the standard
// JSON Schema `additionalProperties` keyword even though OpenAI-compatible
// providers accept and use it. Remove that keyword only from schema nodes in
// the Gemini wire representation; the ToolRegistry's canonical schema remains
// unchanged for validation and for OpenRouter/DeepSeek serialization.
nlohmann::json gemini_function_schema(const nlohmann::json& schema) {
    if (!schema.is_object())
        return schema;

    nlohmann::json result = schema;
    result.erase("additionalProperties");

    for (const auto key : {"properties", "$defs", "defs", "definitions"}) {
        const auto it = result.find(key);
        if (it == result.end() || !it->is_object())
            continue;
        for (auto& [_, child] : it->items())
            child = gemini_function_schema(child);
    }

    for (const auto key : {"items", "not"}) {
        const auto it = result.find(key);
        if (it != result.end() && it->is_object())
            *it = gemini_function_schema(*it);
    }

    for (const auto key : {"prefixItems", "anyOf", "oneOf", "allOf"}) {
        const auto it = result.find(key);
        if (it == result.end() || !it->is_array())
            continue;
        for (auto& child : *it)
            child = gemini_function_schema(child);
    }

    return result;
}

} // namespace

nlohmann::json serialize_gemini_tools(const ToolRegistry& tools) {
    if (tools.empty())
        return nlohmann::json::array();

    auto declarations = tools.definitions_json();
    for (auto& declaration : declarations) {
        const auto parameters = declaration.find("parameters");
        if (parameters != declaration.end())
            *parameters = gemini_function_schema(*parameters);
    }
    return nlohmann::json::array({{{"functionDeclarations", std::move(declarations)}}});
}

nlohmann::json build_gemini_payload(const std::string& system_instruction,
                                    const nlohmann::json& contents,
                                    const ToolRegistry& tools) {
    nlohmann::json payload = {{"contents", contents}};
    if (!system_instruction.empty()) {
        payload["systemInstruction"] = {{"parts", {{{"text", system_instruction}}}}};
    }
    if (!tools.empty()) {
        payload["tools"] = serialize_gemini_tools(tools);
    }
    return payload;
}

void parse_gemini_stream_chunk(std::string_view event,
                               std::string& text_accumulator,
                               nlohmann::json& model_response_parts,
                               nlohmann::json& function_calls,
                               const TextStreamCallback& on_text) {
    try {
        const auto parsed = nlohmann::json::parse(event);
        const auto packets = parsed.is_array() ? parsed : nlohmann::json::array({parsed});
        for (const auto& packet : packets) {
            const auto candidates_it = packet.find("candidates");
            if (candidates_it == packet.end() || !candidates_it->is_array() || candidates_it->empty())
                continue;
            const auto& first_candidate = (*candidates_it)[0];
            const auto content_it = first_candidate.find("content");
            if (content_it == first_candidate.end() || !content_it->is_object())
                continue;
            const auto parts_it = content_it->find("parts");
            if (parts_it == content_it->end() || !parts_it->is_array())
                continue;
            for (const auto& part : *parts_it) {
                model_response_parts.push_back(part);
                if (part.contains("text")) {
                    const auto chunk = part.at("text").get<std::string>();
                    text_accumulator += chunk;
                    if (on_text)
                        on_text(chunk);
                }
                if (part.contains("functionCall"))
                    function_calls.push_back(part.at("functionCall"));
            }
        }
    } catch (const std::exception&) {
        // Ignore malformed transient SSE data
    }
}

nlohmann::json serialize_deepseek_tools(const ToolRegistry& tools) {
    auto result = nlohmann::json::array();
    for (const auto& definition : tools.definitions_json()) {
        result.push_back({{"type", "function"}, {"function", definition}});
    }
    return result;
}

nlohmann::json build_deepseek_payload(const std::string& model,
                                      const nlohmann::json& messages,
                                      const ToolRegistry& tools) {
    nlohmann::json payload = {
        {"model", model},
        {"messages", messages},
        {"max_tokens", 2048},
        {"stream", true}
    };
    if (!tools.empty()) {
        payload["tools"] = serialize_deepseek_tools(tools);
        payload["tool_choice"] = "auto";
    }
    return payload;
}

namespace {

void accumulate_tool_call_delta(nlohmann::json& tool_calls, const nlohmann::json& change, std::size_t default_index) {
    if (!change.is_object())
        return;

    std::size_t index = default_index;
    if (change.contains("index") && change["index"].is_number()) {
        const auto raw_index = change["index"].get<std::int64_t>();
        if (raw_index >= 0) {
            index = static_cast<std::size_t>(raw_index);
        }
    }

    while (tool_calls.size() <= index) {
        tool_calls.push_back(nlohmann::json{
            {"id", ""},
            {"type", "function"},
            {"function", {{"name", ""}, {"arguments", ""}}}
        });
    }

    auto& call = tool_calls.at(index);
    if (!call.is_object()) {
        call = nlohmann::json{
            {"id", ""},
            {"type", "function"},
            {"function", {{"name", ""}, {"arguments", ""}}}
        };
    }
    if (!call.contains("function") || !call["function"].is_object()) {
        call["function"] = nlohmann::json{{"name", ""}, {"arguments", ""}};
    }
    if (!call["function"].contains("name") || !call["function"]["name"].is_string()) {
        call["function"]["name"] = "";
    }
    if (!call["function"].contains("arguments") || !call["function"]["arguments"].is_string()) {
        call["function"]["arguments"] = "";
    }
    if (!call.contains("id") || !call["id"].is_string()) {
        call["id"] = "";
    }
    if (!call.contains("type") || !call["type"].is_string()) {
        call["type"] = "function";
    }

    if (change.contains("id") && change["id"].is_string()) {
        const auto& id_chunk = change["id"].get_ref<const std::string&>();
        if (!id_chunk.empty()) {
            std::string current_id = call["id"].get<std::string>();
            current_id += id_chunk;
            call["id"] = std::move(current_id);
        }
    }

    if (change.contains("type") && change["type"].is_string()) {
        const auto& type_chunk = change["type"].get_ref<const std::string&>();
        if (!type_chunk.empty()) {
            call["type"] = type_chunk;
        }
    }

    if (change.contains("function") && change["function"].is_object()) {
        const auto& function = change["function"];
        if (function.contains("name") && function["name"].is_string()) {
            const auto& name_chunk = function["name"].get_ref<const std::string&>();
            if (!name_chunk.empty()) {
                std::string current_name = call["function"]["name"].get<std::string>();
                current_name += name_chunk;
                call["function"]["name"] = std::move(current_name);
            }
        }

        if (function.contains("arguments")) {
            if (function["arguments"].is_string()) {
                const auto& args_chunk = function["arguments"].get_ref<const std::string&>();
                if (!args_chunk.empty()) {
                    std::string current_args = call["function"]["arguments"].get<std::string>();
                    current_args += args_chunk;
                    call["function"]["arguments"] = std::move(current_args);
                }
            } else if (!function["arguments"].is_null()) {
                std::string current_args = call["function"]["arguments"].get<std::string>();
                if (current_args.empty()) {
                    call["function"]["arguments"] = function["arguments"].dump();
                }
            }
        }
    }
}

} // namespace

void parse_deepseek_stream_chunk(std::string_view event,
                                 std::string& text_accumulator,
                                 nlohmann::json& tool_calls,
                                 const TextStreamCallback& on_text) {
    if (event == "[DONE]")
        return;
    try {
        const auto parsed = nlohmann::json::parse(event);
        const auto packets = parsed.is_array() ? parsed : nlohmann::json::array({parsed});
        for (const auto& packet : packets) {
            const auto choices_it = packet.find("choices");
            if (choices_it == packet.end() || !choices_it->is_array() || choices_it->empty())
                continue;
            const auto delta_it = (*choices_it)[0].find("delta");
            if (delta_it == (*choices_it)[0].end() || !delta_it->is_object())
                continue;
            const auto& delta = *delta_it;
            if (delta.contains("content") && !delta.at("content").is_null()) {
                const auto chunk = delta.at("content").get<std::string>();
                text_accumulator += chunk;
                if (on_text)
                    on_text(chunk);
            }
            for (const auto& change : delta.value("tool_calls", nlohmann::json::array())) {
                const auto index = change.value("index", 0U);
                while (tool_calls.size() <= index) {
                    tool_calls.push_back(
                        {{"id", ""},
                         {"type", "function"},
                         {"function", {{"name", ""}, {"arguments", ""}}}});
                }
                auto& call = tool_calls.at(index);
                if (change.contains("id"))
                    call["id"] = change.at("id");
                if (change.contains("type"))
                    call["type"] = change.at("type");
                if (change.contains("function")) {
                    const auto& function = change.at("function");
                    if (function.contains("name"))
                        call["function"]["name"] = function.at("name");
                    if (function.contains("arguments")) {
                        call["function"]["arguments"] =
                            call["function"]["arguments"].get<std::string>() +
                            function.at("arguments").get<std::string>();
                    }
                }
            }
        }
    } catch (const std::exception&) {
        // Ignore malformed transient SSE data
    }
}

std::string join_api_path(std::string_view prefix, std::string_view path) {
    if (prefix.empty()) return std::string(path);
    if (path.empty()) return std::string(prefix);
    if (prefix.ends_with('/') && path.starts_with('/')) {
        return std::string(prefix.substr(0, prefix.size() - 1)) + std::string(path);
    }
    if (!prefix.ends_with('/') && !path.starts_with('/')) {
        return std::string(prefix) + "/" + std::string(path);
    }
    return std::string(prefix) + std::string(path);
}

std::vector<OpenRouterModelEntry> parse_openrouter_model_catalog(std::string_view body) {
    const auto document = nlohmann::json::parse(body);
    std::vector<OpenRouterModelEntry> entries;
    const nlohmann::json* items = nullptr;
    if (document.is_object()) {
        const auto data_it = document.find("data");
        if (data_it != document.end() && data_it->is_array()) {
            items = &(*data_it);
        } else {
            const auto models_it = document.find("models");
            if (models_it != document.end() && models_it->is_array()) {
                items = &(*models_it);
            }
        }
    } else if (document.is_array()) {
        items = &document;
    }
    if (!items)
        return entries;

    for (const auto& item : *items) {
        if (!item.is_object())
            continue;
        std::string id;
        if (item.contains("id") && item["id"].is_string()) {
            id = item["id"].get<std::string>();
        } else if (item.contains("name") && item["name"].is_string()) {
            id = item["name"].get<std::string>();
        }
        if (id.empty())
            continue;

        if (item.contains("architecture") && item["architecture"].is_object()) {
            const auto& arch = item["architecture"];
            if (arch.contains("modality") && arch["modality"].is_string()) {
                const auto modality = arch["modality"].get<std::string>();
                if (modality.find("embeddings") != std::string::npos ||
                    modality.find("image->image") != std::string::npos) {
                    continue;
                }
            }
        }

        ModelCapabilities caps;
        if (item.contains("supported_parameters") && item["supported_parameters"].is_array()) {
            const auto& params = item["supported_parameters"];
            if (!params.empty()) {
                bool has_tools = false;
                for (const auto& p : params) {
                    if (p.is_string() && p.get<std::string>() == "tools") {
                        has_tools = true;
                        break;
                    }
                }
                caps.supports_tools = has_tools;
            }
        }

        std::string name = item.value("name", id);
        std::string desc = item.value("description", "");

        entries.push_back(OpenRouterModelEntry{
            .id = std::move(id),
            .name = std::move(name),
            .description = std::move(desc),
            .capabilities = caps
        });
    }

    std::ranges::sort(entries, [](const auto& a, const auto& b) { return a.id < b.id; });
    return entries;
}

nlohmann::json serialize_openrouter_tools(const ToolRegistry& tools) {
    auto result = nlohmann::json::array();
    for (const auto& definition : tools.definitions_json()) {
        result.push_back({{"type", "function"}, {"function", definition}});
    }
    return result;
}

nlohmann::json build_openrouter_payload(const std::string& model,
                                        const nlohmann::json& messages,
                                        const ToolRegistry& tools,
                                        bool supports_tools) {
    nlohmann::json payload = {
        {"model", model},
        {"messages", messages},
        {"stream", true}
    };
    if (supports_tools && !tools.empty()) {
        payload["tools"] = serialize_openrouter_tools(tools);
        payload["tool_choice"] = "auto";
    }
    return payload;
}

void parse_openrouter_stream_chunk(std::string_view event,
                                   std::string& text_accumulator,
                                   nlohmann::json& tool_calls,
                                   const TextStreamCallback& on_text,
                                   std::string* stream_error) {
    if (event == "[DONE]") return;
    // Callback exceptions are not JSON parse errors and must not be swallowed.
    const auto parsed = nlohmann::json::parse(event, nullptr, false);
    if (parsed.is_discarded()) return;
    const auto packets = parsed.is_array() ? parsed : nlohmann::json::array({parsed});
    for (const auto& packet : packets) {
        if (!packet.is_object()) continue;
        if (packet.contains("error") && !packet["error"].is_null()) {
            if (stream_error && stream_error->empty()) {
                const auto& error = packet["error"];
                if (error.is_string()) *stream_error = error.get<std::string>();
                else if (error.is_object() && error.contains("message") && error["message"].is_string())
                    *stream_error = error["message"].get<std::string>();
                else *stream_error = "Upstream reported an error.";
            }
            continue;
        }
        const auto choices_it = packet.find("choices");
        if (choices_it == packet.end() || !choices_it->is_array() || choices_it->empty()) continue;
        // ARN requests one completion; do not merge alternative choices together.
        const auto& choice = choices_it->front();
        if (!choice.is_object()) continue;
        const auto delta_it = choice.find("delta");
        const auto message_it = choice.find("message");
        const bool snapshot = message_it != choice.end() && message_it->is_object();
        const nlohmann::json* delta = snapshot ? &*message_it :
            (delta_it != choice.end() && delta_it->is_object() ? &*delta_it : nullptr);
        std::string chunk;
        const auto append_content = [&](const nlohmann::json& content) {
            if (content.is_string()) chunk += content.get_ref<const std::string&>();
            else if (content.is_array()) {
                for (const auto& part : content) {
                    if (!part.is_object()) continue;
                    const auto type = part.find("type");
                    const auto value = part.find("text");
                    if (type != part.end() && type->is_string() && *type == "text" &&
                        value != part.end() && value->is_string()) chunk += value->get_ref<const std::string&>();
                }
            }
        };
        if (delta && delta->contains("content")) append_content((*delta)["content"]);
        else if (choice.contains("text")) append_content(choice["text"]);
        if (snapshot && chunk.starts_with(text_accumulator)) chunk.erase(0, text_accumulator.size());
        if (!chunk.empty()) {
            text_accumulator += chunk;
            if (on_text) on_text(chunk);
        }
        if (delta && delta->contains("tool_calls") && (*delta)["tool_calls"].is_array()) {
            std::size_t index = 0;
            for (const auto& change : (*delta)["tool_calls"]) {
                if (snapshot) {
                    // A complete message replaces its calls, rather than concatenating their IDs twice.
                    if (index == 0) tool_calls = nlohmann::json::array();
                }
                accumulate_tool_call_delta(tool_calls, change, index++);
            }
        }
    }
}

} // namespace arn::core::detail
