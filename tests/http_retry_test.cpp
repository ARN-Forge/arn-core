#include "arn/core/net/http_client.hpp"
#include "arn/core/net/model_parser.hpp"

#include <algorithm>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

} // namespace

int main() {
    try {
        // Test 1: should_retry
        {
            // Null response (transport failure) should retry
            httplib::Result null_result;
            check(arn::core::net::should_retry(null_result),
                  "Null result should trigger retry");

            // HTTP 5xx and 429 should retry
            for (const int status : {429, 500, 502, 503, 504}) {
                httplib::Response resp;
                resp.status = status;
                httplib::Result res(std::make_unique<httplib::Response>(resp), httplib::Error::Success);
                check(arn::core::net::should_retry(res),
                      "Status should trigger retry");
            }

            // HTTP 200, 400, 401, 403, 404 should not retry
            for (const int status : {200, 201, 400, 401, 403, 404}) {
                httplib::Response resp;
                resp.status = status;
                httplib::Result res(std::make_unique<httplib::Response>(resp), httplib::Error::Success);
                check(!arn::core::net::should_retry(res),
                      "Status should NOT trigger retry");
            }
        }

        // Test 2: retry_delay with Retry-After header
        {
            httplib::Response resp;
            resp.status = 429;
            resp.set_header("Retry-After", "5");
            httplib::Result res(std::make_unique<httplib::Response>(resp), httplib::Error::Success);

            const auto delay = arn::core::net::retry_delay(res, 0);
            check(delay == std::chrono::seconds(5), "Expected 5 seconds from Retry-After");
        }

        // Test 3: retry_delay exponential backoff + jitter bounds
        {
            httplib::Response resp;
            resp.status = 500;
            httplib::Result res(std::make_unique<httplib::Response>(resp), httplib::Error::Success);

            for (int attempt = 0; attempt < 3; ++attempt) {
                const auto delay = arn::core::net::retry_delay(res, attempt);
                const int base_ms = 500 * (1 << attempt);
                check(delay.count() >= base_ms && delay.count() <= base_ms + 250,
                      "Delay out of expected jitter range");
            }
        }

        // Test 4: execute_with_retry attempts count
        {
            int call_count = 0;
            auto failing_request = [&]() -> httplib::Result {
                ++call_count;
                httplib::Response resp;
                resp.status = 500;
                return httplib::Result(std::make_unique<httplib::Response>(resp), httplib::Error::Success);
            };

            std::atomic_bool cancel{false};
            auto res = arn::core::net::execute_with_retry(failing_request, &cancel, 2);
            check(call_count == 2, "Expected exactly 2 attempts with max_attempts = 2");
            check(res && res->status == 500, "Expected status 500 result");
        }

        // Test 5: execute_with_retry cancellation
        {
            int call_count = 0;
            std::atomic_bool cancel{true};
            auto request = [&]() -> httplib::Result {
                ++call_count;
                httplib::Response resp;
                resp.status = 500;
                return httplib::Result(std::make_unique<httplib::Response>(resp), httplib::Error::Success);
            };

            arn::core::net::execute_with_retry(request, &cancel, 3);
            check(call_count == 1, "Cancelled request should not retry after first attempt");
        }

        // Test 6: streaming retries report bounded, useful progress
        {
            int call_count = 0;
            bool received_event = false;
            std::string error_body = R"({"error":{"message":"Too many requests"}})";
            std::vector<std::string> progress;
            auto request = [&]() -> httplib::Result {
                ++call_count;
                httplib::Response resp;
                resp.status = 429;
                resp.set_header("Retry-After", "0");
                return httplib::Result(std::make_unique<httplib::Response>(resp),
                                       httplib::Error::Success);
            };
            const auto result = arn::core::net::execute_stream_with_retry(
                request, received_event, error_body, nullptr,
                arn::core::net::ProgressCallback{
                    [&](std::string_view message) { progress.emplace_back(message); }}, 3);
            check(result && result->status == 429 && call_count == 3,
                  "Retryable 429 should stop at the configured attempt limit");
            check(std::ranges::any_of(progress, [](const std::string& message) {
                      return message.find("Rate limited by provider; retrying in 0s") !=
                          std::string::npos;
                  }), "Retry progress should explain the reason and delay");
        }

        // Test 7: exhausted quota is terminal and is not retried
        {
            int call_count = 0;
            bool received_event = false;
            std::string error_body =
                R"({"error":{"message":"You exceeded your current quota. Check billing."}})";
            std::vector<std::string> progress;
            auto request = [&]() -> httplib::Result {
                ++call_count;
                httplib::Response resp;
                resp.status = 429;
                return httplib::Result(std::make_unique<httplib::Response>(resp),
                                       httplib::Error::Success);
            };
            const auto result = arn::core::net::execute_stream_with_retry(
                request, received_event, error_body, nullptr,
                arn::core::net::ProgressCallback{
                    [&](std::string_view message) { progress.emplace_back(message); }}, 3);
            check(result && result->status == 429 && call_count == 1,
                  "Hard quota exhaustion must not be retried");
            check(!arn::core::net::should_retry(result, error_body),
                  "Hard quota response should be terminal");
            check(std::ranges::any_of(progress, [](const std::string& message) {
                      return message.find("quota is exhausted") != std::string::npos;
                  }), "Hard quota progress should explain why no retry occurs");
        }

        // Test 8: parse_model_list in arn::core::net
        {
            const std::string gemini_json = R"({"models":[{"name":"models/gemini-2.5-flash","supportedGenerationMethods":["generateContent"]}]})";
            const auto models = arn::core::net::parse_model_list(gemini_json, true);
            check(models.size() == 1 && models[0] == "gemini-2.5-flash", "parse_model_list failed for Gemini");

            const std::string deepseek_json = R"({"data":[{"id":"deepseek-chat"},{"id":"deepseek-reasoner"}]})";
            const auto ds_models = arn::core::net::parse_model_list(deepseek_json, false);
            check(ds_models.size() == 2 && ds_models[0] == "deepseek-chat", "parse_model_list failed for DeepSeek");
        }

        std::cout << "All HTTP retry and model parser tests passed successfully.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "HTTP retry test failed: " << ex.what() << '\n';
        return 1;
    }
}
