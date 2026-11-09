// Per-request HTTP cancellation: httplib::Request::interrupt in arn-core's
// patched cpp-httplib 0.18.3, and its use by net::stream_post.
//
// Everything runs against local servers on 127.0.0.1; nothing needs network
// access. The TLS certificate is generated in memory.
//
// On Windows a blocked socket wait notices the token by itself (50 ms slices).
// On Linux and macOS a blocked wait is woken by stop(), as before; the token is
// still checked before and after every wait on all platforms.

#include "arn/core/net/http_client.hpp"

#include <openssl/evp.h>
#include <openssl/x509.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

#ifdef _WIN32
constexpr bool sliced_waits = true;
#else
constexpr bool sliced_waits = false;
#endif

// "Ended promptly": the target is about 100 ms; this bound absorbs CI noise.
constexpr double prompt_limit_ms = 250.0;
// Long enough that reaching it can never be mistaken for a cancellation.
constexpr time_t long_timeout_s = 20;
constexpr int stream_events = 5000;

int failures = 0;
std::map<std::string, double> worst_ms;

double ms_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::string fmt_ms(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << value << " ms";
    return out.str();
}

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << '\n';
    }
}

void record(const std::string& category, double value) {
    auto& worst = worst_ms[category];
    if (value > worst) worst = value;
}

std::string error_text(const httplib::Result& result) {
    return result ? "HTTP " + std::to_string(result->status) : httplib::to_string(result.error());
}

bool canceled(const httplib::Result& result) {
    return !result && result.error() == httplib::Error::Canceled;
}

bool wait_until(const std::function<bool()>& condition, std::chrono::milliseconds limit) {
    const auto deadline = Clock::now() + limit;
    while (!condition()) {
        if (Clock::now() >= deadline) return false;
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

// A request that never returns cannot be cleaned up safely; end the process.
template <typename T>
T finish(std::future<T>& done, const std::string& what, std::chrono::seconds limit = 60s) {
    if (done.wait_for(limit) != std::future_status::ready) {
        std::cerr << "FAIL: " << what << ": request still running after " << limit.count()
                  << " s\n";
        std::cerr.flush();
        std::_Exit(3);
    }
    return done.get();
}

class Gate {
public:
    void open() {
        {
            std::lock_guard lock(mutex_);
            open_ = true;
        }
        cv_.notify_all();
    }
    bool wait(std::chrono::milliseconds limit = 30000ms) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, limit, [&] { return open_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool open_ = false;
};

// Self-signed EC certificate for 127.0.0.1, created once per process.
class TlsIdentity {
public:
    TlsIdentity() {
        key_ = EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256");
        cert_ = X509_new();
        if (!key_ || !cert_) throw std::runtime_error("could not create the test TLS key");
        X509_set_version(cert_, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(cert_), 1);
        X509_gmtime_adj(X509_getm_notBefore(cert_), -3600);
        X509_gmtime_adj(X509_getm_notAfter(cert_), 86400);
        X509_set_pubkey(cert_, key_);
        // Built separately: OpenSSL 4 returns the certificate's names as const.
        X509_NAME* name = X509_NAME_new();
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>("127.0.0.1"), -1, -1, 0);
        X509_set_subject_name(cert_, name);
        X509_set_issuer_name(cert_, name);
        X509_NAME_free(name);
        if (X509_sign(cert_, key_, EVP_sha256()) <= 0)
            throw std::runtime_error("could not sign the test certificate");
    }
    ~TlsIdentity() {
        X509_free(cert_);
        EVP_PKEY_free(key_);
    }
    TlsIdentity(const TlsIdentity&) = delete;
    TlsIdentity& operator=(const TlsIdentity&) = delete;
    X509* cert() const { return cert_; }
    EVP_PKEY* key() const { return key_; }

private:
    EVP_PKEY* key_ = nullptr;
    X509* cert_ = nullptr;
};

const TlsIdentity& identity() {
    static const TlsIdentity id;
    return id;
}

// Local HTTP(S) server with the routes the tests need:
//   /silent  accepts the request and never answers
//   /mid     sends headers and one event, then nothing
//   /stream  sends stream_events events ("data: N"), with pauses
//   /ok      answers at once; X-Client-Port tells which connection was used
// Blocked handlers are released when the server is destroyed.
class TestServer {
public:
    explicit TestServer(bool tls) : tls_(tls) {
        if (tls)
            server_ = std::make_unique<httplib::SSLServer>(identity().cert(), identity().key());
        else
            server_ = std::make_unique<httplib::Server>();
        if (!server_->is_valid()) throw std::runtime_error("test server is not valid");
        server_->new_task_queue = [] { return new httplib::ThreadPool(32); };

        auto gate = gate_;
        server_->Post("/silent", [this, gate](const httplib::Request&, httplib::Response& res) {
            ++handled_;
            gate->wait();
            res.set_content("late", "text/plain");
        });
        server_->Post("/mid", [this, gate](const httplib::Request&, httplib::Response& res) {
            ++handled_;
            auto sent = std::make_shared<bool>(false);
            res.set_chunked_content_provider(
                "text/event-stream", [gate, sent](std::size_t, httplib::DataSink& sink) {
                    if (!*sent) {
                        *sent = true;
                        const std::string event = "data: first\n\n";
                        return sink.write(event.data(), event.size());
                    }
                    gate->wait();
                    sink.done();
                    return true;
                });
        });
        server_->Post("/stream", [this](const httplib::Request&, httplib::Response& res) {
            ++handled_;
            auto next = std::make_shared<int>(0);
            res.set_chunked_content_provider(
                "text/event-stream", [next](std::size_t, httplib::DataSink& sink) {
                    if (*next >= stream_events) {
                        sink.done();
                        return true;
                    }
                    std::string batch;
                    for (int i = 0; i < 50 && *next < stream_events; ++i, ++*next)
                        batch += "data: " + std::to_string(*next) + "\n\n";
                    // Some pauses are longer than one 50 ms wait slice.
                    if (*next % 1000 == 0)
                        std::this_thread::sleep_for(70ms);
                    else if (*next % 250 == 0)
                        std::this_thread::sleep_for(2ms);
                    return sink.write(batch.data(), batch.size());
                });
        });
        server_->Post("/ok", [this](const httplib::Request& req, httplib::Response& res) {
            ++handled_;
            res.set_header("X-Client-Port", std::to_string(req.remote_port));
            res.set_content("ok", "text/plain");
        });

        port_ = server_->bind_to_any_port("127.0.0.1");
        if (port_ <= 0) throw std::runtime_error("test server could not bind");
        thread_ = std::thread([this] { server_->listen_after_bind(); });
        server_->wait_until_ready();
    }
    ~TestServer() {
        gate_->open();
        server_->stop();
        thread_.join();
    }
    TestServer(const TestServer&) = delete;
    TestServer& operator=(const TestServer&) = delete;

    std::string url() const {
        return std::string(tls_ ? "https" : "http") + "://127.0.0.1:" + std::to_string(port_);
    }
    int handled() const { return handled_.load(); }

private:
    bool tls_;
    std::shared_ptr<Gate> gate_ = std::make_shared<Gate>();
    std::unique_ptr<httplib::Server> server_;
    std::thread thread_;
    int port_ = 0;
    std::atomic_int handled_{0};
};

// Plain TCP listener that accepts connections and never sends anything, and
// counts them. Used for the TLS handshake and for "no connection at all".
class RawListener {
public:
    RawListener() {
        sock_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock_ == INVALID_SOCKET) throw std::runtime_error("raw listener: socket failed");
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(sock_, 16) != 0)
            throw std::runtime_error("raw listener: bind/listen failed");
        socklen_t len = sizeof(addr);
        ::getsockname(sock_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] {
            while (!stop_) {
                if (httplib::detail::select_read(sock_, 0, 20000) > 0) {
                    const auto client = ::accept(sock_, nullptr, nullptr);
                    if (client != INVALID_SOCKET) {
                        std::lock_guard lock(mutex_);
                        accepted_.push_back(client);
                    }
                }
            }
        });
    }
    ~RawListener() {
        stop_ = true;
        thread_.join();
        for (const auto client : accepted_) httplib::detail::close_socket(client);
        httplib::detail::close_socket(sock_);
    }
    RawListener(const RawListener&) = delete;
    RawListener& operator=(const RawListener&) = delete;

    int port() const { return port_; }
    std::size_t accepted() {
        std::lock_guard lock(mutex_);
        return accepted_.size();
    }

private:
    socket_t sock_ = INVALID_SOCKET;
    int port_ = 0;
    std::atomic_bool stop_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::vector<socket_t> accepted_;
};

std::unique_ptr<httplib::Client> make_client(const std::string& url,
                                             time_t read_timeout_s = long_timeout_s,
                                             time_t connect_timeout_s = long_timeout_s) {
    auto client = std::make_unique<httplib::Client>(url);
    if (!client->is_valid()) throw std::runtime_error("invalid client URL " + url);
    client->enable_server_certificate_verification(false);
    client->set_connection_timeout(connect_timeout_s, 0);
    client->set_read_timeout(read_timeout_s, 0);
    client->set_keep_alive(true);
    return client;
}

httplib::Request post(const std::string& path, const std::atomic<bool>* token) {
    httplib::Request request;
    request.method = "POST";
    request.path = path;
    request.headers = {{"Accept", "text/event-stream"}, {"Content-Type", "application/json"}};
    request.body = "{}";
    request.interrupt = token;
    return request;
}

std::string transport(bool tls) { return tls ? "TLS" : "TCP"; }

// 1-4: the request is already blocked waiting for data.
void blocked_wait(bool tls, bool mid_stream, bool with_stop) {
    const std::string name = transport(tls) + (mid_stream ? " mid-stream" : " silent") +
                             (with_stop ? ", token + stop()" : ", token only");
    TestServer server(tls);
    auto client = make_client(server.url());
    std::atomic<bool> token{false};
    std::atomic<bool> got_event{false};
    auto request = post(mid_stream ? "/mid" : "/silent", &token);
    request.content_receiver = [&](const char*, std::size_t, std::uint64_t, std::uint64_t) {
        got_event = true;
        return true;
    };
    auto done = std::async(std::launch::async, [&] { return client->send(request); });
    check(wait_until([&] { return mid_stream ? got_event.load() : server.handled() > 0; }, 5000ms),
          name + ": request did not reach the server");
    std::this_thread::sleep_for(150ms);

    const auto start = Clock::now();
    token.store(true);
    if (with_stop) client->stop();
    const auto result = finish(done, name);
    const double elapsed = ms_since(start);
    record(with_stop ? "blocked wait, token + stop()" : "blocked wait, token only (Windows)", elapsed);
    check(canceled(result), name + ": expected Canceled, got " + error_text(result));
    check(elapsed <= prompt_limit_ms, name + ": took " + fmt_ms(elapsed));
}

// 5: the token is already set when send() starts: no connection is made.
void before_send(bool tls) {
    const std::string name = transport(tls) + " token set before send()";
    RawListener listener;
    auto client = make_client(std::string(tls ? "https" : "http") + "://127.0.0.1:" +
                              std::to_string(listener.port()));
    std::atomic<bool> token{true};
    const auto start = Clock::now();
    const auto result = client->send(post("/ok", &token));
    const double elapsed = ms_since(start);
    record("token set before send()", elapsed);
    check(canceled(result), name + ": expected Canceled, got " + error_text(result));
    check(elapsed <= 50.0, name + ": took " + fmt_ms(elapsed));
    std::this_thread::sleep_for(200ms);
    check(listener.accepted() == 0, name + ": a connection was opened");
    check(token.load(), name + ": httplib changed the token");

    // The same through arn-core: no event, no retry, Canceled.
    int calls = 0;
    bool received_event = false;
    std::string error_body;
    const auto response = arn::core::net::execute_stream_with_retry(
        [&] {
            ++calls;
            return arn::core::net::stream_post(
                *client, "/ok", {}, "{}", [&](std::string_view) { check(false, name + ": event"); },
                error_body, received_event, &token);
        },
        received_event, error_body, &token);
    check(canceled(response), name + " (stream_post): expected Canceled, got " + error_text(response));
    check(calls == 1, name + " (stream_post): request was retried");
}

// 6a: cancellation while the client is connecting (all platforms). The socket
// options callback runs inside the connect step, under httplib's socket mutex;
// it holds the request there while the token is set and stop() is called.
void during_connect(bool tls) {
    const std::string name = transport(tls) + " cancel while connecting";
    TestServer server(tls);
    auto client = make_client(server.url());
    Gate hold;
    std::atomic<bool> connecting{false};
    client->set_socket_options([&](socket_t) {
        connecting = true;
        hold.wait(10000ms);
    });
    std::atomic<bool> token{false};
    auto request = post("/ok", &token);
    auto done = std::async(std::launch::async, [&] { return client->send(request); });
    check(wait_until([&] { return connecting.load(); }, 5000ms), name + ": connect not reached");

    token.store(true);
    auto stopper = std::async(std::launch::async, [&] { client->stop(); });
    std::this_thread::sleep_for(50ms);
    const auto start = Clock::now();
    hold.open();
    const auto result = finish(done, name);
    const double elapsed = ms_since(start);
    record("cancel while connecting", elapsed);
    check(canceled(result), name + ": expected Canceled, got " + error_text(result));
    check(elapsed <= prompt_limit_ms, name + ": took " + fmt_ms(elapsed));
    check(stopper.wait_for(5s) == std::future_status::ready, name + ": stop() did not return");
    check(server.handled() == 0, name + ": the request reached the server");
}

#ifdef _WIN32
// 6b (Windows): the connect wait itself is interruptible. A socket that never
// connects is never reported writable, like a connect that gets no answer.
void connect_wait_windows() {
    const std::string name = "connect wait (Windows)";
    const auto sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    check(sock != INVALID_SOCKET, name + ": socket failed");

    auto start = Clock::now();
    auto error = httplib::detail::wait_until_socket_is_ready(sock, 0, 300000, nullptr);
    double elapsed = ms_since(start);
    check(error == httplib::Error::ConnectionTimeout,
          name + ": control without a token returned " + httplib::to_string(error));
    check(elapsed >= 280.0 && elapsed <= 800.0, name + ": control took " + fmt_ms(elapsed));

    std::atomic<bool> token{false};
    start = Clock::now();
    error = httplib::detail::wait_until_socket_is_ready(sock, 0, 300000, &token);
    elapsed = ms_since(start);
    check(error == httplib::Error::ConnectionTimeout,
          name + ": unset token returned " + httplib::to_string(error));
    check(elapsed >= 280.0 && elapsed <= 800.0, name + ": unset token took " + fmt_ms(elapsed));

    auto done = std::async(std::launch::async, [&] {
        return httplib::detail::wait_until_socket_is_ready(sock, long_timeout_s, 0, &token);
    });
    std::this_thread::sleep_for(200ms);
    start = Clock::now();
    token.store(true);
    error = finish(done, name);
    elapsed = ms_since(start);
    record("connect wait (Windows)", elapsed);
    check(error == httplib::Error::Canceled, name + ": returned " + httplib::to_string(error));
    check(elapsed <= prompt_limit_ms, name + ": took " + fmt_ms(elapsed));
    httplib::detail::close_socket(sock);
}
#endif

// 7: the server accepts TCP but never answers the TLS ClientHello.
// Windows: the handshake wait notices the token. POSIX: the handshake runs
// under httplib's socket mutex, so stop() cannot reach it and the wait ends at
// the connection timeout (2 s here), still reported as Canceled.
void during_tls_handshake() {
    const std::string name = "TLS handshake";
    RawListener listener;
    auto client = make_client("https://127.0.0.1:" + std::to_string(listener.port()),
                              long_timeout_s, sliced_waits ? long_timeout_s : 2);
    std::atomic<bool> token{false};
    auto request = post("/ok", &token);
    auto done = std::async(std::launch::async, [&] { return client->send(request); });
    check(wait_until([&] { return listener.accepted() > 0; }, 5000ms), name + ": no connection");
    std::this_thread::sleep_for(150ms);

    const auto start = Clock::now();
    token.store(true);
    auto stopper = std::async(std::launch::async, [&] { client->stop(); });
    const auto result = finish(done, name);
    const double elapsed = ms_since(start);
    check(canceled(result), name + ": expected Canceled, got " + error_text(result));
    if (sliced_waits) {
        record("TLS handshake (Windows)", elapsed);
        check(elapsed <= prompt_limit_ms, name + ": took " + fmt_ms(elapsed));
    } else {
        record("TLS handshake (POSIX, ends at 2 s connection timeout)", elapsed);
        check(elapsed >= 1000.0 && elapsed <= 4000.0, name + ": took " + fmt_ms(elapsed));
    }
    check(stopper.wait_for(5s) == std::future_status::ready, name + ": stop() did not return");
}

// 8: the token is set while the request thread is between two waits (inside a
// callback). The next wait must see it, without stop(), on every platform.
void between_waits(bool tls, bool in_body) {
    const std::string name = transport(tls) + " token set inside " +
                             (in_body ? "content_receiver" : "response_handler");
    TestServer server(tls);
    auto client = make_client(server.url());
    std::atomic<bool> token{false};
    Gate hold;
    std::atomic<bool> inside{false};
    auto request = post("/mid", &token);
    request.response_handler = [&](const httplib::Response&) {
        if (!in_body) {
            inside = true;
            hold.wait(10000ms);
        }
        return true;
    };
    request.content_receiver = [&](const char*, std::size_t, std::uint64_t, std::uint64_t) {
        if (in_body && !inside) {
            inside = true;
            hold.wait(10000ms);
        }
        return true;
    };
    auto done = std::async(std::launch::async, [&] { return client->send(request); });
    check(wait_until([&] { return inside.load(); }, 5000ms), name + ": callback not reached");
    std::this_thread::sleep_for(50ms);
    token.store(true);
    const auto start = Clock::now();
    hold.open();
    const auto result = finish(done, name);
    const double elapsed = ms_since(start);
    record("token set between two waits", elapsed);
    check(canceled(result), name + ": expected Canceled, got " + error_text(result));
    check(elapsed <= prompt_limit_ms, name + ": took " + fmt_ms(elapsed));
}

// 9: stop() called many times, concurrently, during and after a cancelled
// request; the client stays usable.
void repeated_stop(bool tls) {
    const std::string name = transport(tls) + " repeated stop()";
    TestServer server(tls);
    auto client = make_client(server.url());
    std::atomic<bool> token{false};
    std::atomic<bool> got_event{false};
    auto request = post("/mid", &token);
    request.content_receiver = [&](const char*, std::size_t, std::uint64_t, std::uint64_t) {
        got_event = true;
        return true;
    };
    auto done = std::async(std::launch::async, [&] { return client->send(request); });
    check(wait_until([&] { return got_event.load(); }, 5000ms), name + ": no event");

    const auto start = Clock::now();
    token.store(true);
    std::vector<std::thread> stoppers;
    for (int t = 0; t < 4; ++t)
        stoppers.emplace_back([&] {
            for (int i = 0; i < 25; ++i) {
                client->stop();
                std::this_thread::sleep_for(1ms);
            }
        });
    const auto result = finish(done, name);
    const double elapsed = ms_since(start);
    for (auto& thread : stoppers) thread.join();
    for (int i = 0; i < 3; ++i) client->stop();
    record("repeated stop()", elapsed);
    check(canceled(result), name + ": expected Canceled, got " + error_text(result));
    check(elapsed <= prompt_limit_ms, name + ": took " + fmt_ms(elapsed));

    std::atomic<bool> fresh{false};
    const auto again = client->send(post("/ok", &fresh));
    check(again && again->status == 200, name + ": client not usable afterwards: " + error_text(again));
}

// 10: the client is reused after a cancellation; the cancelled connection is
// closed and a new keep-alive connection works normally.
void reuse_after_cancel(bool tls) {
    const std::string name = transport(tls) + " reuse after cancellation";
    TestServer server(tls);
    auto client = make_client(server.url());

    std::atomic<bool> first_token{false};
    const auto first = client->send(post("/ok", &first_token));
    check(first && first->status == 200, name + ": first request: " + error_text(first));
    const auto first_port = first ? first->get_header_value("X-Client-Port") : std::string();

    std::atomic<bool> cancel_token{false};
    auto request = post("/silent", &cancel_token);
    auto done = std::async(std::launch::async, [&] { return client->send(request); });
    check(wait_until([&] { return server.handled() >= 2; }, 5000ms), name + ": silent not reached");
    std::this_thread::sleep_for(100ms);
    cancel_token.store(true);
    client->stop();
    const auto cancelled = finish(done, name);
    check(canceled(cancelled), name + ": expected Canceled, got " + error_text(cancelled));

    std::atomic<bool> next_token{false};
    const auto next = client->send(post("/ok", &next_token));
    const auto again = client->send(post("/ok", &next_token));
    check(next && next->status == 200 && next->body == "ok", name + ": next: " + error_text(next));
    check(again && again->status == 200, name + ": again: " + error_text(again));
    if (next && again) {
        const auto next_port = next->get_header_value("X-Client-Port");
        check(next_port != first_port, name + ": the cancelled connection was reused");
        check(again->get_header_value("X-Client-Port") == next_port,
              name + ": keep-alive did not work after the cancellation");
    }
}

// 11: two tokens do not affect each other; httplib never writes a token.
void token_isolation() {
    const std::string name = "token isolation";
    TestServer server(false);
    auto client = make_client(server.url());
    std::atomic<bool> a{true};
    std::atomic<bool> b{false};
    const auto ra = client->send(post("/ok", &a));
    const auto rb = client->send(post("/ok", &b));
    check(canceled(ra), name + ": A expected Canceled, got " + error_text(ra));
    check(rb && rb->status == 200, name + ": B: " + error_text(rb));
    check(a.load() && !b.load(), name + ": httplib changed a token");

    // Two clients at once: cancelling A leaves B's stream intact.
    TestServer other(true);
    auto client_a = make_client(server.url());
    auto client_b = make_client(other.url());
    std::atomic<bool> token_a{false};
    std::atomic<bool> token_b{false};
    std::atomic<bool> a_event{false};
    auto request_a = post("/mid", &token_a);
    request_a.content_receiver = [&](const char*, std::size_t, std::uint64_t, std::uint64_t) {
        a_event = true;
        return true;
    };
    int b_events = 0;
    bool b_received = false;
    std::string b_error;
    auto done_a = std::async(std::launch::async, [&] { return client_a->send(request_a); });
    auto done_b = std::async(std::launch::async, [&] {
        return arn::core::net::stream_post(*client_b, "/stream", {}, "{}",
                                           [&](std::string_view) { ++b_events; }, b_error,
                                           b_received, &token_b);
    });
    check(wait_until([&] { return a_event.load(); }, 5000ms), name + ": A not streaming");
    token_a.store(true);
    client_a->stop();
    const auto result_a = finish(done_a, name + " A");
    const auto result_b = finish(done_b, name + " B");
    check(canceled(result_a), name + ": concurrent A: " + error_text(result_a));
    check(result_b && result_b->status == 200 && b_events == stream_events,
          name + ": concurrent B: " + error_text(result_b) + ", events " + std::to_string(b_events));
    check(!token_b.load(), name + ": B's token changed");
}

// 12: requests without a token behave as before.
void no_token() {
    const std::string name = "no token";
    TestServer server(false);
    auto client = make_client(server.url(), 1);
    const auto ok = client->send(post("/ok", nullptr));
    check(ok && ok->status == 200, name + ": /ok: " + error_text(ok));
    const auto start = Clock::now();
    const auto silent = client->send(post("/silent", nullptr));
    const double elapsed = ms_since(start);
    check(!silent && silent.error() == httplib::Error::Read,
          name + ": expected Read error, got " + error_text(silent));
    check(elapsed >= 900.0 && elapsed <= 2500.0, name + ": read timeout took " + fmt_ms(elapsed));
}

// 13: an unset token does not shorten the read timeout.
void read_timeout_kept(bool tls, bool mid_stream) {
    const std::string name = transport(tls) + (mid_stream ? " mid-stream" : " silent") +
                             " read timeout with unset token";
    TestServer server(tls);
    auto client = make_client(server.url(), 2);
    std::atomic<bool> token{false};
    auto request = post(mid_stream ? "/mid" : "/silent", &token);
    const auto start = Clock::now();
    const auto result = client->send(request);
    const double elapsed = ms_since(start);
    record("read timeout of 2 s with unset token", elapsed);
    check(!result && result.error() == httplib::Error::Read,
          name + ": expected Read error, got " + error_text(result));
    check(elapsed >= 1950.0 && elapsed <= 3500.0, name + ": took " + fmt_ms(elapsed));
    check(!token.load(), name + ": httplib changed the token");
}

// 14: a long SSE stream through arn-core arrives complete and in order.
void large_stream(bool tls) {
    const std::string name = transport(tls) + " large SSE stream";
    TestServer server(tls);
    auto client = make_client(server.url());
    std::atomic<bool> token{false};
    std::vector<int> seen;
    seen.reserve(stream_events);
    bool received = false;
    std::string error_body;
    const auto start = Clock::now();
    const auto result = arn::core::net::stream_post(
        *client, "/stream", {}, "{}",
        [&](std::string_view event) { seen.push_back(std::stoi(std::string(event))); },
        error_body, received, &token);
    const double elapsed = ms_since(start);
    check(result && result->status == 200, name + ": " + error_text(result));
    bool in_order = static_cast<int>(seen.size()) == stream_events;
    for (int i = 0; in_order && i < stream_events; ++i) in_order = seen[i] == i;
    check(in_order, name + ": got " + std::to_string(seen.size()) + " events, in order: " +
                        (in_order ? "yes" : "no"));
    record("large SSE stream (5000 events)", elapsed);
}

// arn-core: stream_post cancelled while blocked, the way ARN cancels (token,
// then stop()); execute_stream_with_retry must not retry.
void stream_post_cancel(bool tls) {
    const std::string name = transport(tls) + " stream_post cancel";
    TestServer server(tls);
    auto client = make_client(server.url());
    std::atomic<bool> token{false};
    std::atomic<int> events{0};
    int calls = 0;
    bool received = false;
    std::string error_body;
    auto done = std::async(std::launch::async, [&] {
        return arn::core::net::execute_stream_with_retry(
            [&] {
                ++calls;
                return arn::core::net::stream_post(*client, "/mid", {}, "{}",
                                                   [&](std::string_view) { ++events; },
                                                   error_body, received, &token);
            },
            received, error_body, &token);
    });
    check(wait_until([&] { return server.handled() > 0; }, 5000ms), name + ": not reached");
    std::this_thread::sleep_for(150ms);
    const auto start = Clock::now();
    token.store(true);
    client->stop();
    const auto result = finish(done, name);
    const double elapsed = ms_since(start);
    record("stream_post, token + stop()", elapsed);
    check(canceled(result), name + ": expected Canceled, got " + error_text(result));
    check(elapsed <= prompt_limit_ms, name + ": took " + fmt_ms(elapsed));
    check(calls == 1, name + ": request was retried");
}

// Races: cancel (token, then stop()) at random moments from just before the
// request starts until it is streaming.
void cancel_races(bool tls, int iterations) {
    const std::string name = transport(tls) + " cancel race";
    std::mt19937 rng(tls ? 9001 : 9000);
    std::uniform_int_distribution<int> delay_us(0, 3000);
    double worst = 0;
    int wrong = 0;
    for (int i = 0; i < iterations; ++i) {
        TestServer server(tls);
        auto client = make_client(server.url());
        std::atomic<bool> token{false};
        auto request = post("/mid", &token);
        auto done = std::async(std::launch::async, [&] { return client->send(request); });
        const auto spin_until = Clock::now() + std::chrono::microseconds(delay_us(rng));
        while (Clock::now() < spin_until) {
        }
        const auto start = Clock::now();
        token.store(true);
        client->stop();
        const auto result = finish(done, name);
        const double elapsed = ms_since(start);
        if (elapsed > worst) worst = elapsed;
        if (!canceled(result)) {
            ++wrong;
            std::cerr << name << " iteration " << i << ": " << error_text(result) << '\n';
        }
    }
    record("random cancel race, token + stop()", worst);
    check(wrong == 0, name + ": " + std::to_string(wrong) + " results were not Canceled");
    check(worst <= 1000.0, name + ": slowest took " + fmt_ms(worst));
}

} // namespace

int main() {
    std::thread([] {
        std::this_thread::sleep_for(240s);
        std::cerr << "FAIL: HTTP interrupt test exceeded 240 s\n";
        std::cerr.flush();
        std::_Exit(4);
    }).detach();

    try {
        for (const bool tls : {false, true}) {
            for (const bool mid : {false, true}) {
                if (sliced_waits) blocked_wait(tls, mid, false);
                blocked_wait(tls, mid, true);
            }
            before_send(tls);
            during_connect(tls);
            between_waits(tls, false);
            between_waits(tls, true);
            repeated_stop(tls);
            reuse_after_cancel(tls);
            read_timeout_kept(tls, tls);
            large_stream(tls);
            stream_post_cancel(tls);
            cancel_races(tls, 100);
        }
#ifdef _WIN32
        connect_wait_windows();
#endif
        during_tls_handshake();
        token_isolation();
        no_token();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: exception: " << error.what() << '\n';
        return 1;
    }

    std::cout << "Waits: " << (sliced_waits ? "50 ms slices (Windows)" : "single wait, stop() wakes it")
              << "\nSlowest result per case:\n";
    for (const auto& [category, value] : worst_ms)
        std::cout << "  " << std::left << std::setw(56) << category << fmt_ms(value) << '\n';
    if (failures != 0) {
        std::cerr << failures << " HTTP interrupt check(s) failed\n";
        return 1;
    }
    std::cout << "All HTTP interrupt tests passed.\n";
    return 0;
}
