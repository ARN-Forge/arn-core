# cpp-httplib patch

arn-core uses [cpp-httplib](https://github.com/yhirose/cpp-httplib) v0.18.3
(MIT licence, notice at the top of `httplib.h`) with one local patch:
`v0.18.3-arn-interrupt.patch`.

| | |
|---|---|
| Upstream tag | `v0.18.3`, commit `a7bc00e3307fecdb4d67545e93be7b88cfb1e186` |
| `httplib.h` before the patch (LF) | SHA-256 `a0a0c13dc086663863dbe6730a19716f7d3744904b6205496e8301750814dbd5` |
| `httplib.h` after the patch (LF) | SHA-256 `bf66c9bb2ce9319bceb73fd8a24553a247d5419c81b98b3c0739a40095c3a441` |

`cmake/ArnCoreHttplib.cmake` fetches the tag, checks the first hash, applies
the patch into the build tree (the fetched sources stay untouched) and checks
the second hash. Any mismatch stops the configure step. The installed package
ships the patched header under `include/arn/core/third_party/cpp-httplib/`.

## Why

On Windows, `ClientImpl::stop()` calls `shutdown()`, which does not wake a
thread blocked in `select()` or `WSAPoll()`. A cancelled AI request therefore
kept waiting for the full read timeout (90 s in arn-core).

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
- On Windows only, those waits run in slices of
  `CPPHTTPLIB_INTERRUPT_CHECK_INTERVAL_USECOND` (50 ms) up to the same total
  timeout. On Linux and macOS each wait stays a single wait, and
  `ClientImpl::stop()` wakes a blocked read as before.
- `CPPHTTPLIB_ARN_INTERRUPT_PATCH` marks the patched header.
  `include/arn/core/net/http_client.hpp` refuses to compile without it.

Servers, `ClientImpl::stop()` and requests without a token behave as before.

## Updating cpp-httplib

Re-create the patch against the new pristine `httplib.h` (LF), then update the
version, commit and both hashes in `cmake/ArnCoreHttplib.cmake` and here.
`tests/http_interrupt_test.cpp` covers the behaviour.
