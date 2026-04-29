# coro

An **experimental** C++20 library for coroutines, asynchronous execution, and async I/O. Inspired by the **Sender/Receiver** composition model proposed in [P2300](https://wg21.link/p2300) (`std::execution`), targeting C++26.

> **Note:** This is a learning and experimentation project. It is not intended for production use. The API may change without notice.

## Features

- **`task<T>`** — a lazy, move-only coroutine type, directly `co_await`-able
- **Combinators** — `then`, `when_all` for composing async pipelines with pipe syntax
- **Schedulers** — `inline_scheduler` (synchronous), `thread_pool_scheduler`, `io_uring_scheduler` (with `async_read` / `async_write` / `async_timeout`)
- **`sync_wait`** — blocking await to bridge async code to synchronous callers
- **Sender/Receiver protocol** — CPOs (`connect`, `start`, `set_value`, `set_error`, `set_stopped`) and concepts (`sender`, `receiver`, `operation_state`)
- **Exception propagation** — errors flow through the pipeline automatically
- **Header-only** — no separate build step needed for the library itself (io_uring requires Linux)

## Requirements

- C++20 with coroutine support
- GCC 11+ or Clang 14+
- Linux (for `io_uring_scheduler` with `IORING_OP_READ` / `WRITE` / `TIMEOUT`)

## Build

```bash
mkdir build && cd build
cmake .. -DCORO_BUILD_TESTS=ON
make -j$(nproc)
```

## Tests

```bash
cd build && ctest --output-on-failure
```

## Quick Start

```cpp
#include <coro/coro.hpp>
using namespace coro;

// Define a coroutine task
task<int> add_one(int x) {
  co_return x + 1;
}

task<int> main_task() {
  int result = co_await add_one(41);
  co_return result;
}

// Execute synchronously
int main() {
  auto result = sync_wait(main_task());  // 42
}
```

### Pipe syntax with combinators

```cpp
auto t = simple_int_task() | then([](int x) { return x * 2; });
auto result = sync_wait(std::move(t));  // 84
```

### Parallel composition

```cpp
auto result = sync_wait(when_all(int_10(), int_20()));
// result == std::make_tuple(10, 20)
```

### Scheduler — control where work runs

A scheduler represents an execution context. Calling `schedule()` returns a sender that completes on that context.

```cpp
// Inline: completes immediately on current thread
inline_scheduler sync;
sync_wait(sync.schedule());

// Thread pool: runs on a worker thread
thread_pool_scheduler pool(4);
auto t = pool.schedule() | then([] { return std::this_thread::get_id(); });
auto tid = sync_wait(std::move(t));  // tid != main thread
```

### io_uring async I/O

On Linux, `io_uring_scheduler` provides true async I/O — pipe, file, timer operations:

```cpp
io_uring_scheduler uring(256);

int fds[2];
pipe2(fds, O_CLOEXEC);

// Write into a pipe via io_uring
const char* msg = "hello";
auto n = sync_wait(uring.async_write(fds[1], msg, 5));

// Read back
char buf[16] = {};
auto r = sync_wait(uring.async_read(fds[0], buf, sizeof(buf)));
// buf == "hello"

// Timeout
sync_wait(uring.async_timeout(std::chrono::milliseconds(50)));
```

## Project Structure

```
include/coro/
├── coro.hpp                ── Aggregate header (include this one)
├── task.hpp                ── task<T> coroutine type + promise_type
├── awaitable.hpp           ── Awaitable traits and helpers
├── sender.hpp              ── Sender concepts and CPOs
├── receiver.hpp            ── Receiver concepts and CPOs
├── then.hpp                ── then combinator (transform)
├── when_all.hpp            ── when_all combinator (parallel join)
├── sync_wait.hpp           ── sync_wait blocking utility
├── scheduler.hpp           ── schedule CPO + scheduler concept
├── inline_scheduler.hpp    ── synchronous scheduler
├── thread_pool_scheduler.hpp ── thread pool scheduler
├── io_uring_scheduler.hpp  ── io_uring scheduler + async_read/write/timeout
└── config.hpp              ── Library configuration

tests/
├── test_task.cpp                   ── task, then, when_all, sender/receiver
├── test_inline_scheduler.cpp       ── inline scheduler
├── test_thread_pool_scheduler.cpp  ── thread pool scheduler
└── test_io_uring_scheduler.cpp     ── io_uring scheduler + real I/O tests
```
