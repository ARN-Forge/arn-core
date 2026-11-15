// Cancellation of requests to servers that stall, with the token alone.
//
// Complements http_interrupt_test.cpp. These servers stall in the places where
// a request used to stay blocked inside a socket call that never checked the
// token: a TLS record that has only partly arrived (SSL_read() blocked in
// recv()), a peer that stops reading a large request body (send() blocked),
// and a slow name lookup (getaddrinfo()). No test calls ClientImpl::stop():
// on Windows shutdown() wakes neither recv() nor select(), so the token must
// be enough on its own.
//
// Everything runs against local servers on 127.0.0.1; nothing needs network
// access. The TLS certificate is generated in memory.

#include "arn/core/net/http_client.hpp"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
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

// Target: cancellation completes within about one second. Local runs take
// about one 50 ms wait slice; the bound leaves room for loaded CI machines.
constexpr double cancel_limit_ms = 1000.0;
// Long enough that reaching it can never be mistaken for a cancellation.
constexpr time_t long_timeout_s = 20;

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

void set_recv_timeout(socket_t sock, int ms) {
#ifdef _WIN32
    const DWORD value = static_cast<DWORD>(ms);
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&value), sizeof(value));
#else
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

// The last recv() ended at the SO_RCVTIMEO set above, not with an error.
bool recv_timed_out() {
#ifdef _WIN32
    return WSAGetLastError() == WSAETIMEDOUT;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

bool send_all(socket_t sock, const char* data, std::size_t size) {
    while (size > 0) {
        const auto n = httplib::detail::send_socket(sock, data, size, 0);
        if (n <= 0) return false;
        data += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

// Small fixed socket buffers. Without them Windows grows a loopback send
// buffer far enough to take a whole large body from a peer that never reads,
// and the client never blocks sending.
constexpr int small_buffer_bytes = 64 * 1024;

void set_buffer(socket_t sock, int option) {
    const int value = small_buffer_bytes;
    setsockopt(sock, SOL_SOCKET, option, reinterpret_cast<const char*>(&value), sizeof(value));
}

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

// What a StallServer connection does after accepting (and, for TLS, after
// the handshake). The request itself is never read.
enum class Stall {
    silent,         // send nothing at all
    partial_headers, // send all but the last bytes of the response-header record
    partial_body,   // complete headers and one SSE event, then part of the next record
};

// A server under the test's full control. Plain TCP or TLS. TLS runs through
// memory BIOs, so the test decides exactly which bytes of each TLS record
// reach the socket. Every connection gets its own thread, which holds the
// connection open until the server is destroyed.
class StallServer {
public:
    StallServer(bool tls, Stall stall) : tls_(tls), stall_(stall) {
        if (tls_) {
            ctx_ = SSL_CTX_new(TLS_server_method());
            if (!ctx_ || SSL_CTX_use_certificate(ctx_, identity().cert()) != 1 ||
                SSL_CTX_use_PrivateKey(ctx_, identity().key()) != 1)
                throw std::runtime_error("stall server: TLS context failed");
            // No session tickets: the only records after the handshake are ours.
            SSL_CTX_set_num_tickets(ctx_, 0);
        }
        sock_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock_ == INVALID_SOCKET) throw std::runtime_error("stall server: socket failed");
        set_buffer(sock_, SO_RCVBUF); // inherited by accepted connections
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(sock_, 16) != 0)
            throw std::runtime_error("stall server: bind/listen failed");
        socklen_t len = sizeof(addr);
        ::getsockname(sock_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        accept_thread_ = std::thread([this] {
            while (!stop_) {
                if (httplib::detail::select_read(sock_, 0, 20000) <= 0) continue;
                const auto client = ::accept(sock_, nullptr, nullptr);
                if (client == INVALID_SOCKET) continue;
                std::lock_guard lock(mutex_);
                clients_.push_back(client);
                workers_.emplace_back([this, client] { serve(client); });
            }
        });
    }
    ~StallServer() {
        stop_ = true;
        accept_thread_.join();
        for (auto& worker : workers_) worker.join();
        for (const auto client : clients_) httplib::detail::close_socket(client);
        httplib::detail::close_socket(sock_);
        if (ctx_) SSL_CTX_free(ctx_);
    }
    StallServer(const StallServer&) = delete;
    StallServer& operator=(const StallServer&) = delete;

    std::string url() const {
        return std::string(tls_ ? "https" : "http") + "://127.0.0.1:" + std::to_string(port_);
    }
    int accepted() const { return accepted_.load(); }
    // Set once the connection has sent everything it will ever send.
    int stalled() const { return stalled_.load(); }

private:
    static constexpr const char* headers_ =
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
        "Transfer-Encoding: chunked\r\n\r\n";
    static constexpr const char* first_chunk_ = "d\r\ndata: first\n\n\r\n";
    static constexpr const char* second_chunk_ = "e\r\ndata: second\n\n\r\n";

    void serve(socket_t client) {
        ++accepted_;
        set_recv_timeout(client, 100);
        if (!tls_) {
            if (stall_ == Stall::partial_headers) {
                const std::string h = headers_;
                send_all(client, h.data(), h.size() - 8);
            } else if (stall_ == Stall::partial_body) {
                const std::string data = std::string(headers_) + first_chunk_;
                send_all(client, data.data(), data.size());
                const std::string second = second_chunk_;
                send_all(client, second.data(), second.size() / 2);
            }
            ++stalled_;
            hold();
            return;
        }

        SSL* ssl = SSL_new(ctx_);
        BIO* in = BIO_new(BIO_s_mem());
        BIO* out = BIO_new(BIO_s_mem());
        SSL_set_bio(ssl, in, out);
        SSL_set_accept_state(ssl);
        const auto flush = [&] {
            std::vector<char> buffer(16384);
            int n;
            while ((n = BIO_read(out, buffer.data(), static_cast<int>(buffer.size()))) > 0)
                if (!send_all(client, buffer.data(), static_cast<std::size_t>(n))) return false;
            return true;
        };
        // One TLS record for `data`, sent whole or minus its last `cut` bytes.
        const auto record_out = [&](const std::string& data, std::size_t cut) {
            if (SSL_write(ssl, data.data(), static_cast<int>(data.size())) <= 0) return false;
            std::string bytes;
            std::vector<char> buffer(16384);
            int n;
            while ((n = BIO_read(out, buffer.data(), static_cast<int>(buffer.size()))) > 0)
                bytes.append(buffer.data(), static_cast<std::size_t>(n));
            return send_all(client, bytes.data(), bytes.size() - cut);
        };

        bool ok = false;
        for (;;) {
            const int rc = SSL_do_handshake(ssl);
            if (!flush()) break;
            if (rc == 1) {
                ok = true;
                break;
            }
            if (SSL_get_error(ssl, rc) != SSL_ERROR_WANT_READ || stop_) break;
            char buffer[16384];
            const auto n = httplib::detail::read_socket(client, buffer, sizeof(buffer), 0);
            if (n > 0) BIO_write(in, buffer, static_cast<int>(n));
            else if (!recv_timed_out())
                break;
        }
        if (ok) {
            if (stall_ == Stall::partial_headers) {
                record_out(headers_, 8);
            } else if (stall_ == Stall::partial_body) {
                record_out(headers_, 0);
                record_out(first_chunk_, 0);
                record_out(second_chunk_, 10);
            }
        }
        ++stalled_;
        hold();
        SSL_free(ssl);
    }

    void hold() const {
        while (!stop_) std::this_thread::sleep_for(10ms);
    }

    bool tls_;
    Stall stall_;
    SSL_CTX* ctx_ = nullptr;
    socket_t sock_ = INVALID_SOCKET;
    int port_ = 0;
    std::atomic_bool stop_{false};
    std::atomic_int accepted_{0};
    std::atomic_int stalled_{0};
    std::thread accept_thread_;
    std::mutex mutex_;
    std::vector<socket_t> clients_;
    std::vector<std::thread> workers_;
};

// A normal HTTP(S) server for completions. /ok answers at once, /echo returns
// the size of the request body, /big returns big_size bytes.
constexpr std::size_t big_size = 8u << 20;

class NormalServer {
public:
    explicit NormalServer(bool tls) : tls_(tls) {
        if (tls)
            server_ = std::make_unique<httplib::SSLServer>(identity().cert(), identity().key());
        else
            server_ = std::make_unique<httplib::Server>();
        if (!server_->is_valid()) throw std::runtime_error("normal server is not valid");
        server_->Post("/ok", [](const httplib::Request&, httplib::Response& res) {
            res.set_content("ok", "text/plain");
        });
        server_->Post("/echo", [](const httplib::Request& req, httplib::Response& res) {
            res.set_content(std::to_string(req.body.size()), "text/plain");
        });
        server_->Post("/big", [](const httplib::Request&, httplib::Response& res) {
            std::string body(big_size, 'x');
            for (std::size_t i = 0; i < body.size(); i += 4096) body[i] = static_cast<char>('a' + (i / 4096) % 26);
            res.set_content(std::move(body), "application/octet-stream");
        });
        port_ = server_->bind_to_any_port("127.0.0.1");
        if (port_ <= 0) throw std::runtime_error("normal server could not bind");
        thread_ = std::thread([this] { server_->listen_after_bind(); });
        server_->wait_until_ready();
    }
    ~NormalServer() {
        server_->stop();
        thread_.join();
    }
    NormalServer(const NormalServer&) = delete;
    NormalServer& operator=(const NormalServer&) = delete;
    std::string url() const {
        return std::string(tls_ ? "https" : "http") + "://127.0.0.1:" + std::to_string(port_);
    }

private:
    bool tls_;
    std::unique_ptr<httplib::Server> server_;
    std::thread thread_;
    int port_ = 0;
};

std::unique_ptr<httplib::Client> make_client(const std::string& url,
                                             time_t read_timeout_s = long_timeout_s,
                                             time_t write_timeout_s = long_timeout_s) {
    auto client = std::make_unique<httplib::Client>(url);
    if (!client->is_valid()) throw std::runtime_error("invalid client URL " + url);
    client->enable_server_certificate_verification(false);
    client->set_connection_timeout(long_timeout_s, 0);
    client->set_read_timeout(read_timeout_s, 0);
    client->set_write_timeout(write_timeout_s, 0);
    client->set_keep_alive(true);
    return client;
}

// A client whose send buffer stays small (see small_buffer_bytes).
std::unique_ptr<httplib::Client> make_sending_client(const std::string& url,
                                                     time_t write_timeout_s) {
    auto client = make_client(url, long_timeout_s, write_timeout_s);
    client->set_socket_options([](socket_t sock) { set_buffer(sock, SO_SNDBUF); });
    return client;
}

httplib::Request post(const std::string& path, const std::atomic<bool>* token,
                      std::string body = "{}") {
    httplib::Request request;
    request.method = "POST";
    request.path = path;
    request.headers = {{"Accept", "text/event-stream"}, {"Content-Type", "application/json"}};
    request.body = std::move(body);
    request.interrupt = token;
    return request;
}

// A 64 MB upload written in 64 KB pieces, the way a content provider streams
// it. (Windows takes one large send() whole whenever its send buffer is not yet
// full, so a single 64 MB body would never block the client.)
httplib::Request upload(const std::atomic<bool>* token) {
    auto request = post("/", token, std::string());
    request.content_length_ = 64u << 20;
    request.content_provider_ = [](std::size_t, std::size_t length, httplib::DataSink& sink) {
        static const std::string piece(64 * 1024, 'x');
        // Returning true leaves a failed write to be reported as Error::Write
        // (a provider returning false is reported as Error::Canceled).
        sink.write(piece.data(), (std::min)(length, piece.size()));
        return true;
    };
    return request;
}

std::string transport(bool tls) { return tls ? "TLS" : "TCP"; }

// Sets the token once `ready` holds and a little time has passed, so the
// request is inside its blocking step; returns how long the request took to
// end after that, and checks it ended Canceled within the limit.
void expect_prompt_cancel(const std::string& name, const std::string& category,
                          std::future<httplib::Result>& done, std::atomic<bool>& token,
                          const std::function<bool()>& ready) {
    check(wait_until(ready, 5000ms), name + ": the stall was not reached");
    std::this_thread::sleep_for(200ms);
    check(done.wait_for(0s) != std::future_status::ready, name + ": request ended by itself");
    const auto start = Clock::now();
    token.store(true);
    const auto result = finish(done, name);
    const double elapsed = ms_since(start);
    record(category, elapsed);
    check(canceled(result), name + ": expected Canceled, got " + error_text(result));
    check(elapsed <= cancel_limit_ms, name + ": took " + fmt_ms(elapsed));
}

// 1. A server that accepts and never answers (no response headers at all).
void never_responds(bool tls) {
    const std::string name = transport(tls) + " server never responds";
    StallServer server(tls, Stall::silent);
    auto client = make_client(server.url());
    std::atomic<bool> token{false};
    auto request = post("/", &token);
    auto done = std::async(std::launch::async, [&] { return client->send(request); });
    expect_prompt_cancel(name, "never responds", done, token,
                         [&] { return server.stalled() > 0; });
}

// 2. Stall in the middle of the response headers. Over TLS, part of the
// header record has arrived, so SSL_read() needs more bytes than the socket has.
void stall_in_headers(bool tls) {
    const std::string name = transport(tls) + " stall inside the response headers";
    StallServer server(tls, Stall::partial_headers);
    auto client = make_client(server.url());
    std::atomic<bool> token{false};
    auto request = post("/", &token);
    auto done = std::async(std::launch::async, [&] { return client->send(request); });
    expect_prompt_cancel(name, "stall inside response headers", done, token,
                         [&] { return server.stalled() > 0; });
}

// 3. Headers and one event arrive, then the body stalls (over TLS inside a
// record), through arn-core's streaming API as providers use it.
void stall_in_stream(bool tls) {
    const std::string name = transport(tls) + " stall inside a streamed body";
    StallServer server(tls, Stall::partial_body);
    auto client = make_client(server.url());
    std::atomic<bool> token{false};
    std::vector<std::string> events;
    std::mutex events_mutex;
    std::atomic<bool> got_event{false};
    int calls = 0;
    bool received = false;
    std::string error_body;
    auto done = std::async(std::launch::async, [&] {
        return arn::core::net::execute_stream_with_retry(
            [&] {
                ++calls;
                return arn::core::net::stream_post(
                    *client, "/", {}, "{}",
                    [&](std::string_view event) {
                        std::lock_guard lock(events_mutex);
                        events.emplace_back(event);
                        got_event = true;
                    },
                    error_body, received, &token);
            },
            received, error_body, &token);
    });
    expect_prompt_cancel(name, "stall inside streamed body", done, token,
                         [&] { return got_event.load() && server.stalled() > 0; });
    std::lock_guard lock(events_mutex);
    check(events.size() == 1 && events.front() == "first",
          name + ": expected the one event before the stall");
    check(calls == 1, name + ": request was retried");
}

// 4. The peer stops reading a large request body: the client is blocked
// sending. The write timeout is long, so only the token can end it.
void stall_while_sending(bool tls) {
    const std::string name = transport(tls) + " peer stops reading the request body";
    StallServer server(tls, Stall::silent);
    auto client = make_sending_client(server.url(), long_timeout_s);
    std::atomic<bool> token{false};
    auto request = upload(&token);
    auto done = std::async(std::launch::async, [&] { return client->send(request); });
    // The send buffers fill within milliseconds; wait well past that.
    expect_prompt_cancel(name, "peer stops reading request body", done, token,
                         [&] { return server.stalled() > 0; });
}

// 5. Without cancellation the timeouts stay what they were: a stall ends with
// a read (or write) error after the configured timeout, not earlier, not later.
void timeouts_kept(bool tls) {
    {
        const std::string name = transport(tls) + " read timeout kept (stall inside headers)";
        StallServer server(tls, Stall::partial_headers);
        auto client = make_client(server.url(), 1);
        std::atomic<bool> token{false};
        const auto start = Clock::now();
        const auto result = client->send(post("/", &token));
        const double elapsed = ms_since(start);
        record("read timeout of 1 s, unset token", elapsed);
        check(!result && result.error() == httplib::Error::Read,
              name + ": expected Read error, got " + error_text(result));
        check(elapsed >= 950.0 && elapsed <= 2500.0, name + ": took " + fmt_ms(elapsed));
    }
    {
        const std::string name = transport(tls) + " write timeout kept (peer stops reading)";
        StallServer server(tls, Stall::silent);
        auto client = make_sending_client(server.url(), 1);
        std::atomic<bool> token{false};
        const auto start = Clock::now();
        const auto result = client->send(upload(&token));
        const double elapsed = ms_since(start);
        record("write timeout of 1 s, unset token", elapsed);
        check(!result && result.error() == httplib::Error::Write,
              name + ": expected Write error, got " + error_text(result));
        check(elapsed >= 950.0 && elapsed <= 4000.0, name + ": took " + fmt_ms(elapsed));
    }
}

// 6. Repeated cancellation and normal completion on one client: cancel a
// stalled request at a random moment, then complete normal requests (large
// bodies both ways, keep-alive) with an unset token, many times over.
void repeated_cancel_and_complete(bool tls, int iterations) {
    const std::string name = transport(tls) + " repeated cancel and complete";
    StallServer stalled(tls, Stall::partial_body);
    NormalServer normal(tls);
    auto stall_client = make_client(stalled.url());
    auto client = make_client(normal.url());
    std::mt19937 rng(tls ? 7001 : 7000);
    std::uniform_int_distribution<int> delay_ms(0, 120);
    double worst = 0;
    int wrong = 0;
    const std::string upload(4u << 20, 'u');
    for (int i = 0; i < iterations; ++i) {
        std::atomic<bool> token{false};
        auto request = post("/", &token);
        auto done = std::async(std::launch::async, [&] { return stall_client->send(request); });
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms(rng)));
        const auto start = Clock::now();
        token.store(true);
        const auto result = finish(done, name);
        const double elapsed = ms_since(start);
        if (elapsed > worst) worst = elapsed;
        if (!canceled(result)) {
            ++wrong;
            std::cerr << name << " iteration " << i << ": " << error_text(result) << '\n';
        }

        std::atomic<bool> unset{false};
        const auto ok = client->send(post("/ok", &unset));
        check(ok && ok->status == 200 && ok->body == "ok", name + ": /ok: " + error_text(ok));
        if (i % 5 == 0) {
            const auto echo = client->send(post("/echo", &unset, upload));
            check(echo && echo->status == 200 && echo->body == std::to_string(upload.size()),
                  name + ": /echo: " + error_text(echo));
            const auto big = client->send(post("/big", &unset));
            bool intact = big && big->status == 200 && big->body.size() == big_size;
            for (std::size_t at = 0; intact && at < big_size; at += 4096)
                intact = big->body[at] == static_cast<char>('a' + (at / 4096) % 26);
            check(intact, name + ": /big: " + error_text(big));
        }
    }
    record("repeated cancel, slowest", worst);
    check(wrong == 0, name + ": " + std::to_string(wrong) + " results were not Canceled");
    check(worst <= cancel_limit_ms, name + ": slowest took " + fmt_ms(worst));
}

#if defined(CPPHTTPLIB_ARN_INTERRUPT_PATCH_LEVEL) && CPPHTTPLIB_ARN_INTERRUPT_PATCH_LEVEL >= 2
// 7. Name lookup: getaddrinfo() itself cannot be interrupted, so a cancelled
// lookup must return at once and leave the helper thread to finish alone.
void name_lookup() {
    const std::string name = "name lookup";
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST;
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto finished = std::make_shared<std::atomic<int>>(0);
    // A resolver that takes 1.5 s and then really resolves 127.0.0.1.
    const auto slow = [calls, finished](const char* node, const char* service,
                                        const addrinfo* h, addrinfo** out) {
        ++*calls;
        std::this_thread::sleep_for(1500ms);
        const int rc = getaddrinfo(node, service, h, out);
        ++*finished;
        return rc;
    };

    // Unset token: the result arrives as usual.
    std::atomic<bool> token{false};
    addrinfo* result = nullptr;
    auto start = Clock::now();
    int rc = httplib::detail::getaddrinfo_interruptible(slow, "127.0.0.1", "80", &hints, &result,
                                                        &token);
    double elapsed = ms_since(start);
    check(rc == 0 && result != nullptr, name + ": unset token: lookup failed");
    check(elapsed >= 1400.0, name + ": unset token returned early: " + fmt_ms(elapsed));
    if (result) freeaddrinfo(result);

    // Token set during the lookup: returns within one slice.
    result = nullptr;
    auto done = std::async(std::launch::async, [&] {
        return httplib::detail::getaddrinfo_interruptible(slow, "127.0.0.1", "80", &hints,
                                                          &result, &token);
    });
    std::this_thread::sleep_for(200ms);
    start = Clock::now();
    token.store(true);
    if (done.wait_for(10s) != std::future_status::ready) {
        std::cerr << "FAIL: " << name << ": cancelled lookup did not return\n";
        std::_Exit(3);
    }
    rc = done.get();
    elapsed = ms_since(start);
    record("name lookup", elapsed);
    check(rc != 0 && result == nullptr, name + ": cancelled lookup reported a result");
    check(elapsed <= cancel_limit_ms, name + ": cancelled lookup took " + fmt_ms(elapsed));

    // Token already set: the resolver is not even called.
    const int before = calls->load();
    rc = httplib::detail::getaddrinfo_interruptible(slow, "127.0.0.1", "80", &hints, &result,
                                                    &token);
    check(rc != 0 && calls->load() == before, name + ": pre-set token still resolved");

    // The abandoned helper finishes on its own and frees its result.
    check(wait_until([&] { return finished->load() == calls->load(); }, 5000ms),
          name + ": abandoned lookup never finished");

    // The real thing, through a client, to a numeric address.
    NormalServer server(false);
    auto client = make_client(server.url());
    std::atomic<bool> unset{false};
    const auto ok = client->send(post("/ok", &unset));
    check(ok && ok->status == 200, name + ": request with token: " + error_text(ok));
    auto by_name = make_client("http://localhost:" + server.url().substr(server.url().rfind(':') + 1));
    // localhost may resolve to ::1 first; httplib then tries 127.0.0.1.
    const auto named = by_name->send(post("/ok", &unset));
    check(named && named->status == 200, name + ": localhost with a token: " + error_text(named));
}
#endif

} // namespace

// Usage: arn_core_http_stall_test [filter]. With a filter, only the cases whose
// name ("TCP never_responds", "TLS stall_in_headers", "name_lookup", ...)
// contains it run.
int main(int argc, char** argv) {
    const std::string filter = argc > 1 ? argv[1] : "";
    std::thread([] {
        std::this_thread::sleep_for(240s);
        std::cerr << "FAIL: HTTP stall test exceeded 240 s\n";
        std::cerr.flush();
        std::_Exit(4);
    }).detach();

    const auto run = [&](const std::string& name, const std::function<void()>& test) {
        if (!filter.empty() && name.find(filter) == std::string::npos) return;
        std::cout << "case " << name << std::endl;
        test();
    };
    try {
        for (const bool tls : {false, true}) {
            const auto t = transport(tls) + " ";
            run(t + "never_responds", [&] { never_responds(tls); });
            run(t + "stall_in_headers", [&] { stall_in_headers(tls); });
            run(t + "stall_in_stream", [&] { stall_in_stream(tls); });
            run(t + "stall_while_sending", [&] { stall_while_sending(tls); });
            run(t + "timeouts_kept", [&] { timeouts_kept(tls); });
            run(t + "repeated_cancel_and_complete", [&] { repeated_cancel_and_complete(tls, 30); });
        }
#if defined(CPPHTTPLIB_ARN_INTERRUPT_PATCH_LEVEL) && CPPHTTPLIB_ARN_INTERRUPT_PATCH_LEVEL >= 2
        run("name_lookup", [&] { name_lookup(); });
#else
        std::cout << "Name lookup case skipped: httplib patch level below 2\n";
#endif
    } catch (const std::exception& error) {
        std::cerr << "FAIL: exception: " << error.what() << '\n';
        return 1;
    }

    std::cout << "Slowest result per case:\n";
    for (const auto& [category, value] : worst_ms)
        std::cout << "  " << std::left << std::setw(40) << category << fmt_ms(value) << '\n';
    if (failures != 0) {
        std::cerr << failures << " HTTP stall check(s) failed\n";
        return 1;
    }
    std::cout << "All HTTP stall tests passed.\n";
    return 0;
}
