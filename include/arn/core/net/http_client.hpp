/**
 * @file http_client.hpp
 * @brief Resilient HTTP client utilities with retry, exponential backoff, and SSE streaming.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <thread>

#include <httplib.h>

namespace arn::core::net {

/// Default maximum number of attempts for transient HTTP requests.
constexpr int max_request_attempts = 3;

/// Callback for reporting status and retry progress to the consumer.
using ProgressCallback = std::function<void(std::string_view message)>;
/// Callback for delivering decoded SSE event chunks.
using EventCallback = std::function<void(std::string_view event_data)>;

/**
 * @brief Evaluates whether an HTTP result is transient and eligible for retry.
 *
 * Returns true for network transport errors (null response) and HTTP status codes 429 and 5xx.
 *
 * @param response httplib::Result returned from a request.
 * @return True if the request should be retried, false otherwise.
 */
[[nodiscard]] bool should_retry(const httplib::Result& response);

/**
 * @brief Computes the backoff duration before the next retry attempt.
 *
 * Respects the `Retry-After` HTTP header if present. Otherwise applies exponential
 * backoff with randomized jitter.
 *
 * @param response Previous HTTP result.
 * @param attempt 0-based attempt index.
 * @return Delay duration in milliseconds.
 */
[[nodiscard]] std::chrono::milliseconds retry_delay(const httplib::Result& response, int attempt);

/**
 * @brief Executes an HTTP request with automatic retry and cooperative cancellation.
 * @tparam RequestFn Callable returning `httplib::Result`.
 * @param request Lambda or functor executing the HTTP request.
 * @param cancel_requested Optional pointer to atomic cancellation flag.
 * @param max_attempts Maximum attempts allowed (default 3).
 * @return Final httplib::Result.
 */
template <typename RequestFn>
auto execute_with_retry(RequestFn&& request, const std::atomic_bool* cancel_requested = nullptr,
                        int max_attempts = max_request_attempts) {
    for (int attempt = 0;; ++attempt) {
        auto response = request();
        if ((cancel_requested && cancel_requested->load()) || !should_retry(response) ||
            attempt + 1 >= max_attempts)
            return response;
        const auto deadline = std::chrono::steady_clock::now() + retry_delay(response, attempt);
        while (std::chrono::steady_clock::now() < deadline) {
            if (cancel_requested && cancel_requested->load())
                return response;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
}

/**
 * @brief Executes a streaming HTTP request with retry logic guarded against partial replays.
 *
 * If any event was already delivered to the user (@c received_event is true), the request
 * is NOT retried to prevent duplicating text output.
 *
 * @tparam RequestFn Callable returning `httplib::Result`.
 * @tparam ProgressFn Callable receiving progress messages.
 * @param request Lambda executing the streaming HTTP request.
 * @param received_event Reference to boolean tracking if any event payload was delivered.
 * @param cancel_requested Optional pointer to atomic cancellation flag.
 * @param on_progress Optional callback for status and retry notices.
 * @param max_attempts Maximum attempts allowed (default 3).
 * @return Final httplib::Result.
 */
template <typename RequestFn, typename ProgressFn = ProgressCallback>
auto execute_stream_with_retry(RequestFn&& request, const bool& received_event,
                               const std::atomic_bool* cancel_requested = nullptr,
                               const ProgressFn& on_progress = {},
                               int max_attempts = max_request_attempts) {
    for (int attempt = 0;; ++attempt) {
        if (on_progress)
            on_progress("Waiting for provider response (attempt " + std::to_string(attempt + 1) +
                        ")");
        auto response = request();
        // Never replay a partially printed answer: retrying then would show
        // duplicated text to the person using ARN.
        if ((cancel_requested && cancel_requested->load(std::memory_order_relaxed)) ||
            received_event || !should_retry(response) || attempt + 1 >= max_attempts ||
            (!response && response.error() == httplib::Error::Read))
            return response;
        // A full idle read timeout already waited 90 seconds. Do not silently repeat it.
        if (on_progress)
            on_progress("Provider unavailable or rate-limited; waiting before retry");
        const auto deadline = std::chrono::steady_clock::now() + retry_delay(response, attempt);
        while (std::chrono::steady_clock::now() < deadline) {
            if (cancel_requested && cancel_requested->load())
                return response;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
}

/**
 * @brief Performs a POST request with chunked SSE response decoding and cancellation support.
 * @param client httplib client instance.
 * @param path Request path.
 * @param headers HTTP request headers.
 * @param body Request body payload.
 * @param on_event Callback receiving decoded SSE events.
 * @param error_body Output string populated with server error body if status is not 2xx.
 * @param received_event Output flag set to true once at least one event is parsed.
 * @param cancel_requested Optional cancellation token.
 * @return httplib::Result indicating outcome.
 */
httplib::Result stream_post(httplib::Client& client, const std::string& path,
                            const httplib::Headers& headers, const std::string& body,
                            const EventCallback& on_event,
                            std::string& error_body, bool& received_event,
                            const std::atomic_bool* cancel_requested = nullptr);

} // namespace arn::core::net
