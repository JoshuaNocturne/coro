# coro

[English](README.md)

一个**实验性**的 C++20 协程、异步执行与异步 I/O 库。受 [P2300](https://wg21.link/p2300)（`std::execution`）提出的 **Sender/Receiver** 组合模型启发，面向 C++26。

> **注意**：本项目仅用于学习与实验，不适用于生产环境。API 可能随时变更。

## 特性

- **`task<T>`** — 惰性、仅移动的协程类型，可直接 `co_await`
- **组合器** — `then`、`when_all`、`let_value`，以管道语法组合异步流水线
- **调度器** — `inline_scheduler`（同步）、`thread_pool_scheduler`、`io_uring_scheduler`（含 `async_read` / `async_write` / `async_timeout`）
- **co_await 任意 sender** — 通用 `sender_awaiter` 将 Sender/Receiver 协议桥接到协程；调度器 sender 提供自定义 awaiter 以保证线程恢复
- **`sync_wait`** — 阻塞等待，将异步代码桥接到同步调用者
- **Sender/Receiver 协议** — CPO（`connect`、`start`、`set_value`、`set_error`、`set_stopped`）与概念（`sender`、`receiver`、`operation_state`）
- **无竞态完成** — 三态原子 CAS 协议消除双重恢复的未定义行为（P2583R0）
- **异常传播** — 错误与取消（`set_stopped`）自动沿流水线传播
- **纯头文件** — 无需单独构建库本身（io_uring 需要 Linux）

## 环境要求

- 支持 coroutine 的 C++20 编译器
- GCC 11+ 或 Clang 14+
- Linux（用于 `io_uring_scheduler` 的 `IORING_OP_READ` / `WRITE` / `TIMEOUT`）

## 构建

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## 测试

```bash
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

## 快速上手

```cpp
#include <coro/coro.hpp>
using namespace coro;

// 定义协程任务
task<int> add_one(int x) {
  co_return x + 1;
}

task<int> main_task() {
  int result = co_await add_one(41);
  co_return result;
}

// 同步执行
int main() {
  auto result = sync_wait(main_task());  // 42
}
```

### 管道语法与组合器

```cpp
auto t = simple_int_task() | then([](int x) { return x * 2; });
auto result = sync_wait(std::move(t));  // 84
```

### 并行组合

```cpp
auto result = sync_wait(when_all(int_10(), int_20()));
// result == std::make_tuple(10, 20)
```

### 调度器 — 控制代码执行位置

调度器代表一个执行上下文。调用 `schedule()` 返回一个在该上下文上完成的 sender。

```cpp
// 内联调度器：在当前线程上立即完成
inline_scheduler sync;
sync_wait(sync.schedule());

// 线程池调度器：在工作线程上执行
thread_pool_scheduler pool(4);
auto t = pool.schedule() | then([] { return std::this_thread::get_id(); });
auto tid = sync_wait(std::move(t));  // tid != 主线程
```

### P2300 风格：在协程中 co_await schedule()

使用 `co_await pool.schedule()` 将协程切换到工作线程。自定义 `operator co_await` 保证协程在线程池上恢复。

```cpp
task<int> compute_on_pool(thread_pool_scheduler& pool) {
  co_await pool.schedule();   // 挂起，在工作线程上恢复
  // 下面的代码都在线程池的工作线程上执行
  co_return 42;
}

thread_pool_scheduler pool(4);
auto result = sync_wait(compute_on_pool(pool));  // 42
```

### let_value — 在调度器上惰性组合任务

`let_value` 惰性地用上游 sender 的值调用一个可调用对象，并连接返回的 sender。结合调度器使用，可在指定上下文上运行 `task<T>`：

```cpp
thread_pool_scheduler pool(2);
auto t = pool.schedule() | let_value([] { return some_task(42); });
auto result = sync_wait(std::move(t));
```

### io_uring 异步 I/O

在 Linux 上，`io_uring_scheduler` 提供真正的异步 I/O — 管道、文件、定时器操作：

```cpp
io_uring_scheduler uring(256);

int fds[2];
pipe2(fds, O_CLOEXEC);

// 通过 io_uring 写入管道
const char* msg = "hello";
auto n = sync_wait(uring.async_write(fds[1], msg, 5));

// 读回数据
char buf[16] = {};
auto r = sync_wait(uring.async_read(fds[0], buf, sizeof(buf)));
// buf == "hello"

// 超时
sync_wait(uring.async_timeout(std::chrono::milliseconds(50)));
```

## 项目结构

```
include/coro/
├── coro.hpp                  ── 聚合头文件（只需包含此文件）
├── task.hpp                  ── task<T> 协程类型 + promise_type
├── awaitable.hpp             ── Awaitable 特性与辅助工具
├── sender.hpp                ── Sender 概念、CPO、sender_awaiter、operator co_await
├── receiver.hpp              ── Receiver 概念与 CPO
├── then.hpp                  ── then 组合器（变换）
├── when_all.hpp              ── when_all 组合器（并行汇合）
├── let_value.hpp             ── let_value 组合器（惰性绑定）
├── sync_wait.hpp             ── sync_wait 阻塞工具
├── scheduler.hpp             ── schedule CPO + scheduler 概念
├── inline_scheduler.hpp      ── 同步调度器（自定义 co_await）
├── thread_pool_scheduler.hpp ── 线程池调度器（自定义 co_await）
├── io_uring_scheduler.hpp    ── io_uring 调度器 + async_read/write/timeout
└── config.hpp                ── 库配置

tests/
├── test_task.cpp                   ── task、then、when_all、sender/receiver
├── test_inline_scheduler.cpp       ── 内联调度器
├── test_thread_pool_scheduler.cpp  ── 线程池 + P2300 风格 co_await 测试
└── test_io_uring_scheduler.cpp     ── io_uring 调度器 + 真实 I/O 测试
```

## 文档

- [**task\<T\> 使用指南**](docs/task_usage.zh.md)
- [**Sender/Receiver 使用指南**](docs/sender_receiver_usage.zh.md)
