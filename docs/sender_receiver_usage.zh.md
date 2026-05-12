# Sender / Receiver 协议参考

本文档从**底层协议视角**介绍 `coro` 库的 Sender/Receiver 模型。如果你只是想用协程写异步代码，请先看 [`task_usage.zh.md`](task_usage.zh.md)。本文档面向需要**实现自定义 sender/receiver** 或深入理解协议机制的用户。

---

## 1. 核心协议

Sender/Receiver 定义了异步操作的三方协作契约：

| 角色 | 职责 | 生命周期 |
|------|------|----------|
| **Sender** | 描述一份尚未执行的异步工作 | 创建 → connect → 销毁 |
| **Receiver** | 接收异步操作的完成信号 | 随 operation state 一起存活 |
| **Operation State** | `connect(sender, receiver)` 的产物，持有资源，通过 `start()` 启动 | start() 后直到完成信号发出 |

完成信号**有且仅有三种**，且**恰好调用一次**：

```cpp
coro::set_value(receiver, args...);   // 成功
coro::set_error(receiver, exception_ptr); // 失败
coro::set_stopped(receiver);          // 取消/停止
```

---

## 2. 自定义 Receiver

Receiver 是完成信号的接收端。实现一个自定义 receiver 只需提供三个方法：

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

约束：
- `set_error` 和 `set_stopped` 必须是 `noexcept`
- `set_value` 的签名决定该 receiver 能接收什么类型的值
- 若只接收 `void`，使用 `receiver_of<R>` 概念（无参数 `set_value()`）

---

## 3. 自定义 Sender

Sender 是异步工作的描述。最小实现包含 `value_type` 和 `connect` 方法：

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
            // 模拟异步完成：直接调用 set_value
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

使用：

```cpp
auto sender = delayed_int_sender{};
auto op = coro::connect(std::move(sender), logging_receiver{});
coro::start(op);   // 输出: got value: 42
```

---

## 4. 组合器机制

组合器不是魔法——它们就是返回新 sender 的普通函数。内部通过**嵌套 receiver** 拦截上游完成信号，处理后转发给下游。

### 4.1 `then` — 拦截并转换结果

`then_sender` 持有上游 sender 和转换函数。其内部 receiver 的 `set_value` 执行函数后，将新结果转发给外层 receiver。

```cpp
// 伪代码展示 then 的核心逻辑
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

### 4.2 `let_value` — 基于结果生成新 Sender

与 `then` 的区别：`let_value` 的函数**返回一个 sender**，需要先 `connect` 这个新生成的 sender，再 `start` 它。这是异步版本的 monadic bind。

```cpp
// 伪代码展示 let_value 的两阶段执行
struct let_value_receiver {
    void set_value(auto&& v) {
        try {
            auto new_sender = func_(std::forward<decltype(v)>(v));  // 生成新 sender
            phase2_op_ = coro::connect(std::move(new_sender), phase2_receiver{parent_});
            coro::start(*phase2_op_);  // 启动第二阶段
        } catch (...) {
            coro::set_error(std::move(outer_), std::current_exception());
        }
    }
};
```

两阶段特点：
- **Phase 1**：上游 sender 完成，拿到结果
- **Phase 2**：用结果调用函数，得到新 sender，连接并启动它
- Phase 2 的结果（无论 `set_value`/`set_error`/`set_stopped`）直接透传给外层 receiver

### 4.3 `when_all` — 并发组合的计数器机制

`when_all` 启动所有子操作，用原子计数器追踪完成情况：

```cpp
// 每个子 receiver 的 set_value 逻辑
template<std::size_t Index>
void set_value(auto&& v) {
    if (op_.error_flag_.load()) return;  // 已有错误，忽略
    std::get<Index>(op_.results_).emplace(std::forward<decltype(v)>(v));
    if (op_.remaining_.fetch_sub(1) == 1) {
        op_.on_complete();  // 最后一个完成，触发汇总
    }
}
```

关键点：
- 启动是串行循环（`start_impl` 折叠表达式），但子操作本身可能挂起/异步
- `error_flag` 确保出错时只保留第一个异常
- 所有结果收集完毕后才调用外层 `set_value(tuple<...>)`

---

## 5. 调度器协议

调度器是一个可以提供 `schedule()` 方法的对象，返回一个 `void` sender，其语义是"在该上下文上完成"。

```cpp
template<typename S>
concept scheduler = requires(S& s) {
    { coro::schedule(s) } -> coro::sender;
};
```

### 5.1 `inline_scheduler`

最简单调度器，`schedule()` 返回的 sender 在 `start()` 时**立即同步**调用 `set_value`：

```cpp
coro::inline_scheduler sched;
auto sender = coro::schedule(sched);
// connect + start 后立即完成，无线程切换
```

### 5.2 `thread_pool_scheduler`

将工作投递到线程池队列：

```cpp
coro::thread_pool_scheduler pool(4);
auto sender = coro::schedule(pool);
// connect + start 后，set_value 在线程池的某个工作线程上调用
```

### 5.3 `io_uring_scheduler`

基于 Linux io_uring 提供异步 I/O 能力：

```cpp
coro::io_uring_scheduler uring(256);
auto sender = coro::schedule(uring);     // 在 uring 线程上完成
auto read_sender = uring.async_read(fd, buf, len);  // 异步读 sender
```

#### 执行器委托

默认情况下，完成回调（`on_complete`）在事件循环线程上**内联执行**——即调用 `io_uring_wait_cqe` 的同一线程。这对于轻量工作没问题，但如果协程恢复开销较大（例如触发更多异步操作），可能会阻塞 CQE 处理。

`io_uring_scheduler` 支持**执行器**参数，将协程恢复委托给用户指定的执行上下文：

```cpp
// 将恢复工作委托给线程池
coro::thread_pool_scheduler pool(4);
coro::io_uring_scheduler uring(256, pool.executor());
```

在此配置下，事件循环只处理 CQE——它捕获 `cqe->res`，然后通过执行器分派 `on_complete(result)`。线程池处理所有协程恢复，保持事件循环的响应性。

关键实现细节：
- `cqe->res` 必须在 `io_uring_cqe_seen` **之前**捕获，因为之后内核可能重用 CQE 槽位
- 默认执行器为 `&detail::inline_execute`（函数指针，利用 `std::function` 的小缓冲优化），在事件循环线程上内联执行
- 执行器签名为 `std::function<void(std::function<void()>)>`

构造函数：
```cpp
// 默认：完成回调在事件循环线程上内联执行
io_uring_scheduler(unsigned entries = 256);

// 带执行器：完成回调通过执行器分派
io_uring_scheduler(unsigned entries,
                   std::function<void(std::function<void()>)> executor);
```

> 调度器本身不负责启动 sender。它只是描述一个执行上下文，真正的启动由 `connect` + `start` 完成。

---

## 6. `sync_wait` 的实现原理

`sync_wait` 是 Sender/Receiver 与阻塞代码的桥梁，实现不依赖协程：

```cpp
// 核心逻辑
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
        void set_error(std::exception_ptr e) noexcept { /* 存异常，通知 */ }
        void set_stopped() noexcept { /* 标记停止，通知 */ }
    };

    shared_state state;
    auto op = coro::connect(std::forward<Sender>(s), sync_wait_receiver{&state});
    coro::start(op);
    // 阻塞等待条件变量
    auto lock = std::unique_lock(state.mtx);
    state.cv.wait(lock, [&] { return state.done; });
    // 返回结果或抛出异常
}
```

要点：
- `sync_wait` 自己就是一个自定义 receiver 的使用范例
- 它创建了 `shared_state` 用于线程间同步
- 启动 sender 后阻塞等待，收到完成信号后返回

---

## 7. `task<T>` 在协议中的位置

`task<T>` 是本库中最常用的 sender 实现之一。它同时满足两个角色：

1. **Awaitable**：可在协程中 `co_await`
2. **Sender**：可 `connect` 到 receiver 并 `start`

`task<T>` 的 Sender 路径内部实现：
- `connect(task, receiver)` 创建一个 `op_state`，保存 receiver 和协程句柄
- `start(op)` resume 协程
- 协程 `co_return` 后，在 `final_suspend` 中调用 receiver 的 `set_value`

> 协程的 `co_await` 路径与 Sender/Receiver 路径是两个独立的入口，但最终都到达同一个 `promise_type` 存储结果。详见 [`task_usage.zh.md`](task_usage.zh.md) 中的执行流程图。

---

## 8. 何时需要自定义 Sender/Receiver

| 场景 | 方案 |
|---|---|
| 已有回调式 API 需要包装 | 自定义 sender，`start()` 中注册回调，回调中调用 `set_value` |
| 需要控制异步资源生命周期 | 自定义 operation state，在析构中释放资源 |
| 实现新的组合算法 | 参考 `then` / `let_value` / `when_all` 的嵌套 receiver 模式 |
| 集成新的底层执行机制 | 自定义 scheduler + sender |

---

## 9. 头文件速查

| 头文件 | 内容 |
|---|---|
| `<coro/sender.hpp>` | `sender` / `operation_state` 概念、`connect`、`start` |
| `<coro/receiver.hpp>` | `receiver` / `receiver_of` 概念、`set_value`/`set_error`/`set_stopped` |
| `<coro/scheduler.hpp>` | `scheduler` 概念、`schedule` CPO |
| `<coro/then.hpp>` | `then` 组合器 |
| `<coro/let_value.hpp>` | `let_value` 组合器 |
| `<coro/when_all.hpp>` | `when_all` 组合器 |
| `<coro/sync_wait.hpp>` | `sync_wait` 同步等待 |
| `<coro/task.hpp>` | `task<T>` 协程 sender |
| `<coro/inline_scheduler.hpp>` | `inline_scheduler` |
| `<coro/thread_pool_scheduler.hpp>` | `thread_pool_scheduler` |
| `<coro/io_uring_scheduler.hpp>` | `io_uring_scheduler` |
| `<coro/coro.hpp>` | 包含以上所有头文件 |

---

## 10. `sender_awaiter` —— 将 Sender 桥接到协程

`task<T>` 有自己的 `operator co_await`，但库还提供了**通用桥接**（`sender_awaiter`），让**任意 sender** 都能在协程中直接 `co_await`。这通过 `<coro/sender.hpp>` 中的 ADL `operator co_await` 实现。

### 10.1 用法

任意 sender 都可以在协程中 `co_await`：

```cpp
task<int> pipeline() {
    // 等待调度器 sender —— 在调度器的上下文上恢复
    co_await pool.schedule();

    // 等待 then 流水线
    int x = co_await some_sender() | then([](int v) { return v * 2; });

    // 等待 when_all
    auto [a, b] = co_await when_all(sender_a(), sender_b());
    co_return a + b;
}
```

通用 `operator co_await` 对已定义自己版本的类型（如 `task<T>`、`thread_pool_scheduler::sender`）自动排除。

### 10.2 三态原子 CAS 协议

`sender_awaiter` 使用无竞态协议避免双重恢复的未定义行为（P2583R0）：

| 状态 | 值 | 含义 |
|------|-----|------|
| `initial` | 0 | `await_suspend` 仍在执行 |
| `suspended` | 1 | `await_suspend` 已返回 `noop_coroutine()`，协程已挂起 |
| `completed` | 2 | receiver 已调用完成信号 |

**协议流程：**

1. **`await_suspend`**：原子 CAS `0→1`。若失败（状态已是 `2`），说明 receiver 先完成了 —— 返回 awaiting handle 进行对称转移。不存在 load 再 CAS 的快速路径；仅通过 CAS 判断结果，消除了 load 与 CAS 之间的竞态窗口。
2. **`receiver`**（通过 `sender_await_complete`）：原子 CAS `0→2`。若失败（状态已是 `1`），说明协程已挂起 —— 在完成线程上调用 `resume()`。

CAS 逻辑被提取为共享的 `sender_await_complete<T>()` 辅助函数，消除 `void` 和非 `void` receiver 特化之间的代码重复：

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

恰好只有一个路径恢复协程。`acq_rel` / `acquire` 内存序保证非原子的 `continuation_` 写操作对恢复线程可见。

### 10.3 调度器 sender 的自定义 `operator co_await`

调度器 sender 可以（也应该）提供自己的 `operator co_await`，以实现优化或语义保证：

- **`inline_scheduler::sender`**：返回 `await_ready() = true`，完全绕过挂起。
- **`thread_pool_scheduler::sender`**：直接将 `resume()` 投递到线程池并返回 `noop_coroutine()`，保证协程在工作线程上恢复，而非对称转移回调用者线程。
- **`io_uring_scheduler::sender`**（以及 `read_sender`/`write_sender`/`timeout_sender`）：**不**提供自定义 `operator co_await`，回退到通用 `sender_awaiter`，使用三态 CAS 协议。当 `io_uring_scheduler` 配置了执行器时，`on_complete` 回调（调用 `set_value`/`set_error`）通过执行器分派，因此协程恢复发生在执行器的线程上而非事件循环线程上。

自定义 awaiter 优先于通用 `sender_awaiter`，这得益于 ADL 版本上的 `requires (! ... operator co_await())` 约束。

### 10.4 `set_stopped` 处理

若 sender 调用 `set_stopped()`（取消），`sender_awaiter` 会存储 `stopped_` 标志。当 `await_resume()` 执行时，抛出 `std::runtime_error("sender stopped")`，确保取消信号不会被静默忽略。
