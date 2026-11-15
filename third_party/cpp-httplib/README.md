# cpp-httplib patch

arn-core uses [cpp-httplib](https://github.com/yhirose/cpp-httplib) v0.18.3
(MIT licence, notice at the top of `httplib.h`) with one local patch:
`v0.18.3-arn-interrupt.patch`.

| | |
|---|---|
| Upstream tag | `v0.18.3`, commit `a7bc00e3307fecdb4d67545e93be7b88cfb1e186` |
| `httplib.h` before the patch (LF) | SHA-256 `a0a0c13dc086663863dbe6730a19716f7d3744904b6205496e8301750814dbd5` |
| `httplib.h` after the patch (LF) | SHA-256 `43f3ac7fcb129e23ca9097b5ecb133852d18963b6ba7494408c06dc8edc54ff1` |

`cmake/ArnCoreHttplib.cmake` fetches the tag, checks the first hash, applies
the patch into the build tree (the fetched sources stay untouched) and checks
the second hash. Any mismatch stops the configure step. The installed package
ships the patched header under `include/arn/core/third_party/cpp-httplib/`.

## Why

On Windows, `ClientImpl::stop()` calls `shutdown()`, which wakes neither a
thread blocked in `select()`/`WSAPoll()` nor one blocked in `recv()`/`send()`.
A cancelled AI request therefore kept waiting for the full read timeout
(90 s in arn-core).

Level 1 of the patch sliced the waits, but a TLS read could still block: once
`select()` reported a partly arrived TLS record, `SSL_read()` waited inside a
blocking `recv()` for the rest, and only the socket timeout (`SO_RCVTIMEO`)
ended it. A blocking `send()` to a peer that stopped reading, and
`getaddrinfo()`, could not be cancelled either.

## What the patch adds

- `httplib::Request::interrupt`: an optional `const std::atomic<bool>*` owned
  by the caller. httplib only reads it and never resets it. Once it is true,
  the request ends with `httplib::Error::Canceled`.
- `ClientImpl::send_()` checks the token before it touches the connection,
  after connecting, and when mapping the final error. An interrupted request
  closes TLS without waiting for the peer's `close_notify`.
- Client socket waits check the token before waiting and after every wait:
  TCP connect, the TLS handshake, and reads and writes on `SocketStream` and
  `SSLSocketStream`. A cancellation that arrives between two waits is
  therefore never lost.
- On every platform those waits run in slices of
  `CPPHTTPLIB_INTERRUPT_CHECK_INTERVAL_USECOND` (50 ms) up to the same total
  timeout, so the token alone ends a request. `ClientImpl::stop()` is not
  needed for cancellation (it still works, as before).
- With a token, the client socket is non-blocking for the whole request
  (`SocketStream` and `SSLSocketStream`; restored to blocking afterwards).
  `recv()`, `send()`, `SSL_read()` and `SSL_write()` then never block:
  `SSL_ERROR_WANT_READ`/`WANT_WRITE` and `EWOULDBLOCK` lead back to the
  interruptible waits. Each wait still lasts at most the read or write timeout,
  so timeouts keep their meaning (no progress for that long).
- With a token, `getaddrinfo()` runs on a helper thread while the request
  waits in slices; a cancelled lookup is abandoned and the helper frees its
  own result (`detail::getaddrinfo_interruptible`).
- `CPPHTTPLIB_ARN_INTERRUPT_PATCH` marks the patched header.
  `include/arn/core/net/http_client.hpp` refuses to compile without it.
  `CPPHTTPLIB_ARN_INTERRUPT_PATCH_LEVEL` is `2` for the behaviour above.

Servers, and requests without a token, behave as upstream.

## Updating cpp-httplib

Re-create the patch against the new pristine `httplib.h` (LF), then update the
version, commit and both hashes in `cmake/ArnCoreHttplib.cmake` and here.
`tests/http_interrupt_test.cpp` and `tests/http_stall_test.cpp` cover the
behaviour.
