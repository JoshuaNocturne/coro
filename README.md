# coro

[中文版](README.zh.md)

An **experimental** C++20 library for coroutines, asynchronous execution, and async I/O. Inspired by the **Sender/Receiver** composition model proposed in [P2300](https://wg21.link/p2300) (`std::execution`), targeting C++26.

> **Note:** This is a learning and experimentation project. It is not intended for production use. The API may change without notice.

## Features

- **`task<T>`** — a lazy, move-only coroutine type, directly `co_await`-able
- **Combinators** — `then`, `when_all`, `let_value` for composing async pipelines with pipe syntax
- **Schedulers** — `inline_scheduler` (synchronous), `thread_pool_scheduler`, `io_uring_scheduler` (with `async_read` / `async_write` / `async_timeout`)
- **`co_await` any sender** — generic `sender_awaiter` bridges the Sender/Receiver protocol into coroutines; scheduler senders provide custom awaiters for thread-guaranteed resumption
- **`sync_wait`** — blocking await to bridge async code to synchronous callers
- **Sender/Receiver protocol** — CPOs (`connect`, `start`, `set_value`, `set_error`, `set_stopped`) and concepts (`sender`, `receiver`, `operation_state`)
- **Race-free completion** — three-state atomic CAS protocol eliminates double-resume UB (P2583R0)
- **Exception propagation** — errors and cancellation (`set_stopped`) flow through the pipeline automatically
- **Header-only** — no separate build step needed for the library itself (io_uring requires Linux)

## Requirements

- C++20 with coroutine support
- GCC 11+ or Clang 14+
- Linux (for `io_uring_scheduler` with `IORING_OP_READ` / `WRITE` / `TIMEOUT`)

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## Tests

```bash
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
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

### P2300 style: co_await schedule() inside a coroutine

Use `co_await pool.schedule()` to transfer a coroutine onto a worker thread. The custom `operator co_await` guarantees resumption on the pool.

```cpp
task<int> compute_on_pool(thread_pool_scheduler& pool) {
  co_await pool.schedule();   // suspend, resume on worker thread
  // Everything below runs on a pool worker
  co_return 42;
}

thread_pool_scheduler pool(4);
auto result = sync_wait(compute_on_pool(pool));  // 42
```

### let_value — lazy task composition on a scheduler

`let_value` lazily invokes a callable with the upstream sender's value and connects the returned sender. Combined with a scheduler, it runs a `task<T>` on a specific context:

```cpp
thread_pool_scheduler pool(2);
auto t = pool.schedule() | let_value([] { return some_task(42); });
auto result = sync_wait(std::move(t));
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
├── coro.hpp                  ── Aggregate header (include this one)
├── task.hpp                  ── task<T> coroutine type + promise_type
├── awaitable.hpp             ── Awaitable traits and helpers
├── sender.hpp                ── Sender concepts, CPOs, sender_awaiter, operator co_await
├── receiver.hpp              ── Receiver concepts and CPOs
├── then.hpp                  ── then combinator (transform)
├── when_all.hpp              ── when_all combinator (parallel join)
├── let_value.hpp             ── let_value combinator (lazy bind)
├── sync_wait.hpp             ── sync_wait blocking utility
├── scheduler.hpp             ── schedule CPO + scheduler concept
├── inline_scheduler.hpp      ── synchronous scheduler (custom co_await)
├── thread_pool_scheduler.hpp ── thread pool scheduler (custom co_await)
├── io_uring_scheduler.hpp    ── io_uring scheduler + async_read/write/timeout
└── config.hpp                ── Library configuration

tests/
├── test_task.cpp                   ── task, then, when_all, sender/receiver
├── test_inline_scheduler.cpp       ── inline scheduler
├── test_thread_pool_scheduler.cpp  ── thread pool + P2300 style co_await tests
└── test_io_uring_scheduler.cpp     ── io_uring scheduler + real I/O tests
```

## Documentation

- [**task\<T\> Usage Guide**](docs/task_usage.en.md)
- [**Sender/Receiver Usage**](docs/sender_receiver_usage.en.md)
