# Sender / Receiver Protocol Reference

This document covers the `coro` library's Sender/Receiver model from a **protocol-level perspective**. If you just want to write async code with coroutines, see [`task_usage.en.md`](task_usage.en.md) first. This document is for users who need to **implement custom senders/receivers** or deeply understand the protocol mechanics.

---

## 1. Core Protocol

Sender/Receiver defines a three-party contract for asynchronous operations:

| Role | Responsibility | Lifetime |
|------|---------------|----------|
| **Sender** | Describes a piece of async work (lazy, not yet executing) | Created → connect → destroyed |
| **Receiver** | Receives the completion signal of the async operation | Lives alongside the operation state |
| **Operation State** | Product of `connect(sender, receiver)`, holds resources, started via `start()` | From `start()` until completion signal is emitted |

There are **exactly three** completion signals, and **exactly one** is called:

```cpp
coro::set_value(receiver, args...);       // success
coro::set_error(receiver, exception_ptr); // failure
coro::set_stopped(receiver);              // cancellation / stopped
```

---

## 2. Custom Receiver

A receiver is the consumer side of the completion signals. Implementing a custom receiver requires only three methods:

```cpp
struct logging_receiver {
    void set_value(int v) {
        std::cout << "got value: " << v << "\n";
    }

    void set_error(std::exception_ptr e) noexcept {
        try { std::rethrow_exception(e); }
        catch (const std::exception& ex) {
            std::cerr << "error: " << ex.what() << "\n";
        }
    }

    void set_stopped() noexcept {
        std::cout << "cancelled\n";
    }
};
```

Constraints:
- `set_error` and `set_stopped` must be `noexcept`
- The `set_value` signature determines what value types this receiver can accept
- For `void`, use the `receiver_of<R>` concept (parameterless `set_value()`)

---

## 3. Custom Sender

A sender is a description of async work. The minimal implementation needs `value_type` and `connect`:

```cpp
class delayed_int_sender {
public:
    using value_type = int;

    template<typename R>
        requires coro::receiver_of<R, int>
    class op_state {
        int value_;
        R receiver_;
    public:
        op_state(int v, R&& r)
            : value_(v), receiver_(std::forward<R>(r)) {}

        void start() noexcept {
            // Simulate async completion: call set_value directly
            coro::set_value(std::move(receiver_), value_);
        }
    };

    template<typename R>
        requires coro::receiver_of<R, int>
    auto connect(R&& r) && {
        return op_state<std::remove_cvref_t<R>>(42, std::forward<R>(r));
    }
};
```

Usage:

```cpp
auto sender = delayed_int_sender{};
auto op = coro::connect(std::move(sender), logging_receiver{});
coro::start(op);   // Output: got value: 42
```

---

## 4. Combinator Mechanics

Combinators are not magic — they are ordinary functions that return new senders. Internally they intercept upstream completion signals via **nested receivers**, process them, and forward to the downstream receiver.

### 4.1 `then` — intercept and transform

`then_sender` holds the upstream sender and a transform function. Its inner receiver's `set_value` executes the function, then forwards the new result to the outer receiver.

```cpp
// Pseudocode showing the core logic of then
struct then_receiver {
    OuterReceiver outer_;
    Func func_;

    void set_value(auto&& v) {
        auto result = func_(std::forward<decltype(v)>(v));
        coro::set_value(std::move(outer_), std::move(result));
    }

    void set_error(auto e) noexcept {
        coro::set_error(std::move(outer_), std::move(e));
    }

    void set_stopped() noexcept {
        coro::set_stopped(std::move(outer_));
    }
};
```

### 4.2 `let_value` — generate a new Sender from the result

Difference from `then`: `let_value`'s function **returns a sender**. The newly generated sender must be `connect`ed and `start`ed. This is the async version of monadic bind.

```cpp
// Pseudocode showing the two-phase execution of let_value
struct let_value_receiver {
    void set_value(auto&& v) {
        try {
            auto new_sender = func_(std::forward<decltype(v)>(v));  // Generate new sender
            phase2_op_ = coro::connect(std::move(new_sender), phase2_receiver{parent_});
            coro::start(*phase2_op_);  // Start phase 2
        } catch (...) {
            coro::set_error(std::move(outer_), std::current_exception());
        }
    }
};
```

Two-phase characteristics:
- **Phase 1**: upstream sender completes, result is obtained
- **Phase 2**: the function is called with the result, yielding a new sender, which is connected and started
- Phase 2's result (whether `set_value`/`set_error`/`set_stopped`) is transparently forwarded to the outer receiver

### 4.3 `when_all` — counter mechanism for concurrent composition

`when_all` starts all sub-operations and tracks completion with an atomic counter:

```cpp
// Each sub-receiver's set_value logic
template<std::size_t Index>
void set_value(auto&& v) {
    if (op_.error_flag_.load()) return;  // Error already present, ignore
    std::get<Index>(op_.results_).emplace(std::forward<decltype(v)>(v));
    if (op_.remaining_.fetch_sub(1) == 1) {
        op_.on_complete();  // Last one to complete, trigger aggregation
    }
}
```

Key points:
- Start is a serial loop (`start_impl` fold expression), but sub-operations themselves may suspend/async
- `error_flag` ensures only the first exception is retained on error
- Outer `set_value(tuple<...>)` is called only after all results are collected

---

## 5. Scheduler Protocol

A scheduler is an object that provides a `schedule()` method, returning a `void` sender whose semantics are "complete on this context."

```cpp
template<typename S>
concept scheduler = requires(S& s) {
    { coro::schedule(s) } -> coro::sender;
};
```

### 5.1 `inline_scheduler`

The simplest scheduler — the sender returned by `schedule()` calls `set_value` **immediately and synchronously** on `start()`:

```cpp
coro::inline_scheduler sched;
auto sender = coro::schedule(sched);
// Completes immediately after connect + start, no thread switch
```

### 5.2 `thread_pool_scheduler`

Dispatches work to a thread pool queue:

```cpp
coro::thread_pool_scheduler pool(4);
auto sender = coro::schedule(pool);
// After connect + start, set_value is called on a worker thread
```

### 5.3 `io_uring_scheduler`

Provides async I/O capabilities based on Linux io_uring:

```cpp
coro::io_uring_scheduler uring(256);
auto sender = coro::schedule(uring);     // Completes on the uring thread
auto read_sender = uring.async_read(fd, buf, len);  // Async read sender
```

#### Executor delegation

By default, completion callbacks (`on_complete`) run **inline on the event loop thread** — the same thread that calls `io_uring_wait_cqe`. This is fine for lightweight work, but if coroutine resumption is expensive (e.g., it triggers more async operations), it can starve CQE processing.

`io_uring_scheduler` supports an **executor** parameter that delegates coroutine resumption to a user-specified execution context:

```cpp
// Delegate resumption to a thread pool
coro::thread_pool_scheduler pool(4);
coro::io_uring_scheduler uring(256, pool.executor());
```

With this configuration, the event loop only processes CQEs — it captures `cqe->res`, then dispatches `on_complete(result)` via the executor. The thread pool handles all coroutine resumption, keeping the event loop responsive.

Key implementation details:
- `cqe->res` is captured **before** `io_uring_cqe_seen`, since the kernel may reuse the CQE slot afterward
- The default executor is `&detail::inline_execute` (a function pointer for `std::function` SBO), which runs the callable inline on the event loop thread
- The executor signature is `std::function<void(std::function<void()>)>`

Constructors:
```cpp
// Default: completions run inline on the event loop thread
io_uring_scheduler(unsigned entries = 256);

// With executor: completions dispatched to the executor
io_uring_scheduler(unsigned entries,
                   std::function<void(std::function<void()>)> executor);
```

> A scheduler itself is not responsible for starting the sender. It only describes an execution context; actual starting is done by `connect` + `start`.

---

## 6. `sync_wait` Implementation Principles

`sync_wait` is the bridge between Sender/Receiver and blocking code. Its implementation does not depend on coroutines:

```cpp
// Core logic
auto sync_wait(Sender&& s) -> sender_value_t<Sender> {
    struct shared_state {
        std::mutex mtx;
        std::condition_variable cv;
        bool done = false;
        std::optional<Value> value;
        std::exception_ptr error;
    };

    struct sync_wait_receiver {
        shared_state* state;
        void set_value(auto&&... args) {
            auto lock = std::lock_guard(state->mtx);
            state->value.emplace(std::forward<decltype(args)>(args)...);
            state->done = true;
            state->cv.notify_one();
        }
        void set_error(std::exception_ptr e) noexcept { /* store error, notify */ }
        void set_stopped() noexcept { /* mark stopped, notify */ }
    };

    shared_state state;
    auto op = coro::connect(std::forward<Sender>(s), sync_wait_receiver{&state});
    coro::start(op);
    // Block waiting on condition variable
    auto lock = std::unique_lock(state.mtx);
    state.cv.wait(lock, [&] { return state.done; });
    // Return result or throw exception
}
```

Key points:
- `sync_wait` itself is an example of custom receiver usage
- It creates `shared_state` for thread synchronization
- After starting the sender, it blocks and waits, returning upon receiving the completion signal

---

## 7. The Role of `task<T>` in the Protocol

`task<T>` is one of the most commonly used sender implementations in this library. It satisfies two roles simultaneously:

1. **Awaitable**: can be `co_await`ed inside coroutines
2. **Sender**: can be `connect`ed to a receiver and `start`ed

`task<T>`'s Sender path internal implementation:
- `connect(task, receiver)` creates an `op_state`, storing the receiver and coroutine handle
- `start(op)` resumes the coroutine
- After `co_return`, the receiver's `set_value` is called in `final_suspend`

> The coroutine's `co_await` path and the Sender/Receiver path are two independent entry points, but both ultimately reach the same `promise_type` to store the result. See the execution flow diagrams in [`task_usage.en.md`](task_usage.en.md).

---

## 8. When to Implement Custom Sender/Receiver

| Scenario | Approach |
|---|---|
| Wrapping an existing callback-based API | Custom sender, register callback in `start()`, call `set_value` from callback |
| Need to control async resource lifetime | Custom operation state, release resources in destructor |
| Implementing a new combinator algorithm | Follow the nested receiver pattern of `then` / `let_value` / `when_all` |
| Integrating a new underlying execution mechanism | Custom scheduler + sender |

---

## 9. Header Quick Reference

| Header | Content |
|---|---|
| `<coro/sender.hpp>` | `sender` / `operation_state` concepts, `connect`, `start` |
| `<coro/receiver.hpp>` | `receiver` / `receiver_of` concepts, `set_value`/`set_error`/`set_stopped` |
| `<coro/scheduler.hpp>` | `scheduler` concept, `schedule` CPO |
| `<coro/then.hpp>` | `then` combinator |
| `<coro/let_value.hpp>` | `let_value` combinator |
| `<coro/when_all.hpp>` | `when_all` combinator |
| `<coro/sync_wait.hpp>` | `sync_wait` synchronous blocking |
| `<coro/task.hpp>` | `task<T>` coroutine sender |
| `<coro/inline_scheduler.hpp>` | `inline_scheduler` |
| `<coro/thread_pool_scheduler.hpp>` | `thread_pool_scheduler` |
| `<coro/io_uring_scheduler.hpp>` | `io_uring_scheduler` |
| `<coro/coro.hpp>` | Includes all headers above |

---

## 10. `sender_awaiter` — Bridging Senders into Coroutines

While `task<T>` has its own `operator co_await`, the library provides a **generic bridge** (`sender_awaiter`) that makes **any sender** directly awaitable inside coroutines. This is implemented via ADL `operator co_await` in `<coro/sender.hpp>`.

### 10.1 Usage

Any sender can be `co_await`ed inside a coroutine:

```cpp
task<int> pipeline() {
    // Await a scheduler sender — resumes on the scheduler's context
    co_await pool.schedule();

    // Await a then pipeline
    int x = co_await some_sender() | then([](int v) { return v * 2; });

    // Await when_all
    auto [a, b] = co_await when_all(sender_a(), sender_b());
    co_return a + b;
}
```

The generic `operator co_await` is excluded for types that already define their own (e.g. `task<T>`, `thread_pool_scheduler::sender`).

### 10.2 Three-state atomic CAS protocol

`sender_awaiter` uses a race-free protocol to avoid double-resume UB (P2583R0):

| State | Value | Meaning |
|-------|-------|---------|
| `initial` | 0 | `await_suspend` is still running |
| `suspended` | 1 | `await_suspend` returned `noop_coroutine()`, coroutine is suspended |
| `completed` | 2 | The receiver has invoked a completion signal |

**The protocol:**

1. **`await_suspend`**: Atomically CAS `0→1`. If it fails (state is already `2`), the receiver completed first — return the awaiting handle for symmetric transfer. There is no load-then-CAS fast path; the CAS alone determines the outcome, eliminating the race window between load and CAS.
2. **`receiver`** (via `sender_await_complete`): Atomically CAS `0→2`. If it fails (state is already `1`), the coroutine is already suspended — call `resume()` on the completing thread.

The CAS logic is extracted into a shared `sender_await_complete<T>()` helper to eliminate duplication between the `void` and non-`void` receiver specializations:

```cpp
template<typename T>
inline void sender_await_complete(sender_await_state<T>* state) {
  int expected = state_initial;
  if (!state->state_.compare_exchange_strong(expected, state_completed,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
    if (expected == state_suspended) {
      state->continuation_.resume();
    }
  }
}
```

Exactly one path resumes the coroutine. The `acq_rel` / `acquire` memory ordering ensures the non-atomic `continuation_` write is visible to the resuming thread.

### 10.3 Custom `operator co_await` for scheduler senders

Scheduler senders can (and should) provide their own `operator co_await` for optimized or semantically-guaranteed behavior:

- **`inline_scheduler::sender`**: Returns `await_ready() = true`, bypassing suspension entirely.
- **`thread_pool_scheduler::sender`**: Enqueues `resume()` directly to the pool and returns `noop_coroutine()`, guaranteeing the coroutine resumes on a worker thread rather than symmetric-transferring back to the caller.
- **`io_uring_scheduler::sender`** (and `read_sender`/`write_sender`/`timeout_sender`): Does **not** provide a custom `operator co_await`. It falls through to the generic `sender_awaiter`, which uses the three-state CAS protocol. When an executor is configured on the `io_uring_scheduler`, the `on_complete` callback (which calls `set_value`/`set_error`) is dispatched via the executor, so coroutine resumption happens on the executor's thread rather than the event loop thread.

Custom awaiters take precedence over the generic `sender_awaiter` thanks to the `requires (! ... operator co_await())` constraint on the ADL version.

### 10.4 `set_stopped` handling

If a sender calls `set_stopped()` (cancellation), `sender_awaiter` stores a `stopped_` flag. When `await_resume()` runs, it throws `std::runtime_error("sender stopped")` so the cancellation signal is not silently ignored.
