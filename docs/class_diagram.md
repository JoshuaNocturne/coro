# coro library class diagrams

---

## 1. Core coroutine type: task<T>

```mermaid
classDiagram
    direction TB

    class task~T~ {
        +value_type = T
        +operator co_await() && -> awaiter
        +connect(R&& r) && -> op_state~R~
        +handle() coroutine_handle
        +operator bool()
    }

    class task_promise_type~T~ {
        +get_return_object() task
        +initial_suspend() suspend_always
        +final_suspend() final_awaiter
        +unhandled_exception()
        +result() T
        +set_continuation(handle)
        +set_notify(ctx, fn)
        -continuation_ handle~void~
        -notify_ctx_ void*
        -notify_fn_ notify_fn_t*
    }

    class task_op_state~R~ {
        -handle_ coroutine_handle
        -receiver_ R
        +start() noexcept
        -notify(ctx, promise) static
    }

    class promise_result_storage~T~ {
        <<detail>>
        +state_ state_t
        +value_ / error_ union
        +emplace_value(v)
        +emplace_error(e)
        +get() T
    }

    class promise_return_base~T~ {
        <<detail>>
        +return_value(U&& v)
    }

    class promise_return_base_void {
        <<detail>>
        +return_void()
    }

    task~T~ *-- task_promise_type~T~ : coroutine promise
    task~T~ *-- task_op_state~R~ : sender path
    task_promise_type~T~ --|> promise_return_base~T~ : inherits
    promise_return_base~T~ --|> promise_result_storage~T~ : inherits
    promise_return_base_void --|> promise_result_storage~void~ : inherits
```

---

## 2. Sender / Receiver protocol

```mermaid
classDiagram
    direction TB

    class sender {
        <<concept>>
        +value_type
        +move_constructible
    }

    class sender_to~S,R~ {
        <<concept>>
        +connect(s, r) -> operation_state
    }

    class receiver {
        <<concept>>
        +set_error(exception_ptr) noexcept
        +set_stopped() noexcept
    }

    class receiver_of~R,Args...~ {
        <<concept>>
        +set_value(Args...) noexcept
    }

    class operation_state {
        <<concept>>
        +start() noexcept
    }

    class connect_t {
        +operator()(S&& s, R&& r)
    }

    class start_t {
        +operator()(O& o)
    }

    class set_value_t {
        +operator()(R&& r, Args&&... args)
    }

    class set_error_t {
        +operator()(R&& r, E&& e)
    }

    class set_stopped_t {
        +operator()(R&& r)
    }

    class sender_traits~S~ {
        +value_type
    }

    sender_to~S,R~ ..> sender : refines
    sender_to~S,R~ ..> receiver : refines
    sender_to~S,R~ ..> operation_state : produces
    receiver_of~R,Args...~ ..> receiver : refines
    connect_t ..> sender_to~S,R~ : uses
    start_t ..> operation_state : uses
```

---

## 3. Combinator: then

```mermaid
classDiagram
    direction TB

    class then_sender~S,F~ {
        -sender_ S
        -func_ F
        +value_type
        +connect(R&& r) && -> op_state~R~
    }

    class then_op_state~R~ {
        -func_ F
        -outer_receiver_ R
        -inner_op_ inner_op_t
        +start() noexcept
    }

    class then_receiver {
        -parent_ op_state*
        +set_value(V&& v)
        +set_value() void-input
        +set_error(e) noexcept
        +set_stopped() noexcept
    }

    class then_fn~F~ {
        -func F
        +operator|(S&& s, then_fn&& self) then_sender
    }

    class then_result~F,T~ {
        <<detail>>
        +type
    }

    then_sender~S,F~ *-- then_op_state~R~ : contains
    then_op_state~R~ *-- then_receiver : contains
    then_fn~F~ ..> then_sender~S,F~ : creates
```

---

## 4. Combinator: when_all

```mermaid
classDiagram
    direction TB

    class when_all_sender~Senders...~ {
        -senders_ tuple
        +value_type = tuple
        +connect(R&& r) && -> op_state~R~
    }

    class when_all_op_state~R~ {
        -receiver_ R
        -results_ results_tuple
        -remaining_ atomic~size_t~
        -error_flag_ atomic~bool~
        -error_ exception_ptr
        -sub_ops_ sub_ops_storage
        +start() noexcept
        +on_complete()
        +collect_results() value_type
    }

    class when_all_sub_receiver~Index,ValueT,OpState~ {
        -op_ OpState&
        +set_value(V&& v)
        +set_value() void-input
        +set_error(e) noexcept
        +set_stopped() noexcept
    }

    when_all_sender~Senders...~ *-- when_all_op_state~R~ : contains
    when_all_op_state~R~ *-- "N" when_all_sub_receiver : contains
```

---

## 5. Schedulers

### 5a. inline_scheduler

```mermaid
classDiagram
    direction TB

    class inline_scheduler {
        +schedule() -> inline_sender
    }

    class inline_sender {
        +value_type = void
        +connect(R&& r) -> op_state~R~
    }

    class inline_op_state~R~ {
        -receiver_ R
        +start() noexcept
    }

    inline_scheduler *-- inline_sender : schedule()
    inline_sender *-- inline_op_state~R~ : contains
```

### 5b. thread_pool_scheduler

```mermaid
classDiagram
    direction TB

    class thread_pool_scheduler {
        +schedule() -> tp_sender
        -state_ shared_ptr~pool_state~
    }

    class pool_state {
        <<detail>>
        +mutex mtx
        +condition_variable cv
        +queue~function~ queue
        +bool stopping
        +vector~thread~ workers
        +enqueue(task)
    }

    class tp_sender {
        +value_type = void
        +connect(R&& r) -> op_handle~R~
    }

    class tp_op_handle~R~ {
        -receiver_ R
        -pool_ shared_ptr~pool_state~
        +start() noexcept
    }

    thread_pool_scheduler *-- pool_state : owns
    thread_pool_scheduler *-- tp_sender : schedule()
    tp_sender *-- tp_op_handle~R~ : contains
    tp_op_handle~R~ ..> pool_state : references
```

### 5c. io_uring_scheduler

```mermaid
classDiagram
    direction TB

    class io_uring_scheduler {
        +schedule() -> sender
        +async_read(fd, buf, nbytes) -> read_sender
        +async_write(fd, buf, nbytes) -> write_sender
        +async_timeout(duration) -> timeout_sender
        -state_ shared_ptr~ring_state~
    }

    class ring_state {
        +io_uring_ring ring
        +mutex submit_mtx
        +thread event_loop_thread
        +atomic~bool~ stopping
        +submit_sq()
        +run_event_loop()
    }

    class op_state_base {
        <<abstract>>
        +on_complete(result) noexcept*
    }

    class io_sender {
        +value_type = void
        +connect(R&& r) -> op_handle~R~
    }

    class read_sender {
        +value_type = size_t
        +connect(R&& r) -> op_handle~R~
    }

    class write_sender {
        +value_type = size_t
        +connect(R&& r) -> op_handle~R~
    }

    class timeout_sender {
        +value_type = void
        +connect(R&& r) -> op_handle~R~
    }

    io_uring_scheduler *-- ring_state : owns
    io_uring_scheduler *-- io_sender : schedule()
    io_uring_scheduler *-- read_sender : async_read()
    io_uring_scheduler *-- write_sender : async_write()
    io_uring_scheduler *-- timeout_sender : async_timeout()
    io_sender --|> op_state_base : dispatches via user_data
    read_sender --|> op_state_base : dispatches via user_data
    write_sender --|> op_state_base : dispatches via user_data
    timeout_sender --|> op_state_base : dispatches via user_data
```

### 5d. sync_wait (generic)

```mermaid
classDiagram
    direction TB

    class sync_wait_receiver~T~ {
        <<detail>>
        -state_ shared_state*
        +set_value(Args&&...)
        +set_error(e) noexcept
        +set_stopped() noexcept
    }

    class shared_state {
        <<detail>>
        +mutex mtx
        +condition_variable cv
        +optional~T~ value
        +exception_ptr error
        +bool done
    }

    sync_wait_receiver~T~ *-- shared_state : references
```

---

## 6. Awaitable utilities

```mermaid
classDiagram
    direction TB

    class awaitable_traits~T~ {
        +awaiter_t
        +await_result_t
    }

    class is_awaitable~T~ {
        +value : bool
    }

    class as_awaitable_wrapper~T~ {
        -value T
        +operator co_await()
    }

    awaitable_traits~T~ ..> is_awaitable~T~ : used by
```

---

## 7. Cross-module dependencies

```mermaid
classDiagram
    direction TB

    class task~T~
    class then_sender~S,F~
    class when_all_sender~Senders...~
    class inline_sender
    class tp_sender
    class io_sender
    class read_sender
    class write_sender
    class timeout_sender

    class sender {
        <<concept>>
    }

    class receiver {
        <<concept>>
    }

    class operation_state {
        <<concept>>
    }

    class set_value_t
    class set_error_t
    class set_stopped_t

    task~T~ ..|> sender : models
    then_sender~S,F~ ..|> sender : models
    when_all_sender~Senders...~ ..|> sender : models
    inline_sender ..|> sender : models
    tp_sender ..|> sender : models
    io_sender ..|> sender : models
    read_sender ..|> sender : models
    write_sender ..|> sender : models
    timeout_sender ..|> sender : models

    task~T~ ..> operation_state : connect() returns
    then_sender~S,F~ ..> operation_state : connect() returns
    when_all_sender~Senders...~ ..> operation_state : connect() returns
    inline_sender ..> operation_state : connect() returns
    tp_sender ..> operation_state : connect() returns
    io_sender ..> operation_state : connect() returns
    read_sender ..> operation_state : connect() returns
    write_sender ..> operation_state : connect() returns
    timeout_sender ..> operation_state : connect() returns

    task~T~ ..> set_value_t : calls via op_state
    task~T~ ..> set_error_t : calls via op_state
    then_sender~S,F~ ..> set_value_t : forwards
    then_sender~S,F~ ..> set_error_t : forwards
    when_all_sender~Senders...~ ..> set_value_t : forwards
    when_all_sender~Senders...~ ..> set_error_t : forwards
```
