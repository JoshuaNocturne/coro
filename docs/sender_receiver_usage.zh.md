# Sender / Receiver 协议参考

本文档从**底层协议视角**介绍 `coro` 库的 Sender/Receiver 模型。如果你只是想用协程写异步代码，请先看 [`task_usage.md`](task_usage.md)。本文档面向需要**实现自定义 sender/receiver** 或深入理解协议机制的用户。

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

> 协程的 `co_await` 路径与 Sender/Receiver 路径是两个独立的入口，但最终都到达同一个 `promise_type` 存储结果。详见 [`task_usage.md`](task_usage.md) 中的执行流程图。

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
