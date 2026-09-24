/**
 * @file confirmation_gate.hpp
 * @brief Thread-safe synchronization gate for asynchronous approval workflows.
 */

#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <random>
#include <string>

namespace arn::core {

/**
 * @brief Thread-safe synchronization primitive for coordinating asynchronous human confirmations.
 *
 * Useful in GUI, RPC, or web-based agent frontends where a tool execution thread must pause
 * until an external event or user interaction answers a specific confirmation token.
 *
 * Workflow:
 * 1. The worker thread calls @c wait(emit, timeout), which generates a unique token string
 *    and passes it to @c emit.
 * 2. The frontend presents the request to the user with the token ID.
 * 3. When the user responds, another thread calls @c answer(id, approved).
 * 4. The worker thread unblocks and receives the boolean outcome.
 */
class ConfirmationGate {
    std::mutex mutex_;
    std::condition_variable cv_;
    const std::string nonce_ =
        std::to_string(std::random_device{}()) + "-" + std::to_string(std::random_device{}());
    unsigned long long sequence_ = 0;
    std::string pending_;
    bool answered_ = false, approved_ = false, cancelled_ = false;

public:
    /**
     * @brief Resets the gate's cancellation flag, allowing new requests to be processed.
     */
    void reset() {
        std::lock_guard lock(mutex_);
        cancelled_ = false;
    }

    /**
     * @brief Cancels any currently pending confirmation wait and wakes waiting threads.
     */
    void cancel() {
        std::lock_guard lock(mutex_);
        cancelled_ = true;
        cv_.notify_all();
    }

    /**
     * @brief Submits an approval or rejection for the currently pending request token.
     * @param id The confirmation request token issued by @c wait().
     * @param approved True to approve the request, false to deny.
     * @return True if the answer was accepted for the matching active token; false otherwise.
     */
    bool answer(const std::string& id, bool approved) {
        std::lock_guard lock(mutex_);
        if (id.empty() || id != pending_ || answered_ || cancelled_)
            return false;
        answered_ = true;
        approved_ = approved;
        cv_.notify_all();
        return true;
    }

    /**
     * @brief Emits a unique request token and blocks until answered, cancelled, or timed out.
     * @tparam Emit Callable type taking `const std::string& token`.
     * @param emit Callback invoked immediately with the newly assigned token ID.
     * @param timeout Maximum duration to wait before timing out (default 120 seconds).
     * @return True if the request was answered and approved within timeout; false otherwise.
     */
    template <class Emit>
    bool wait(Emit emit, std::chrono::milliseconds timeout = std::chrono::seconds(120)) {
        std::unique_lock lock(mutex_);
        if (cancelled_)
            return false;
        pending_ = "change-" + nonce_ + "-" + std::to_string(++sequence_);
        answered_ = approved_ = false;
        emit(pending_);
        cv_.wait_for(lock, timeout, [&] { return answered_ || cancelled_; });
        const bool result = answered_ && approved_ && !cancelled_;
        pending_.clear();
        return result;
    }
};

} // namespace arn::core
