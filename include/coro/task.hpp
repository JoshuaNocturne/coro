#pragma once

/// @file task.hpp
/// task<T> — a lazy, move-only coroutine type that models a Sender.

#include <concepts>
#include <coroutine>
#include <exception>
#include <type_traits>
#include <utility>

#include <coro/awaitable.hpp>
#include <coro/config.hpp>
#include <coro/sender.hpp>

namespace coro {

// Forward declaration
template<typename T>
class task;

// ---------------------------------------------------------------------------
//  Detail: promise result storage
// ---------------------------------------------------------------------------

namespace detail {

/// Storage for non-void results.
template<typename T>
struct promise_result_storage {
  using value_type = T;

  enum class state_t { uninitialized, value, error };

  state_t state_ = state_t::uninitialized;
  union {
    value_type value_;
    std::exception_ptr error_;
  };

  promise_result_storage() noexcept {}
  ~promise_result_storage() {
    switch (state_) {
    case state_t::value: value_.~value_type(); break;
    case state_t::error: error_.~exception_ptr(); break;
    default: break;
    }
  }

  void emplace_value(value_type&& v) noexcept(
      std::is_nothrow_move_constructible_v<value_type>) {
    ::new (std::addressof(value_)) value_type(std::move(v));
    state_ = state_t::value;
  }

  void emplace_value(const value_type& v) noexcept(
      std::is_nothrow_copy_constructible_v<value_type>) {
    ::new (std::addressof(value_)) value_type(v);
    state_ = state_t::value;
  }

  void emplace_error(std::exception_ptr e) noexcept {
    ::new (std::addressof(error_)) std::exception_ptr(std::move(e));
    state_ = state_t::error;
  }

  auto get() -> value_type {
    if (state_ == state_t::error) {
      auto e = std::move(error_);
      error_.~exception_ptr();
      state_ = state_t::uninitialized;
      std::rethrow_exception(std::move(e));
    }
    auto result = std::move(value_);
    value_.~value_type();
    state_ = state_t::uninitialized;
    return result;
  }
};

/// Storage specialization for void results.
template<>
struct promise_result_storage<void> {
  using value_type = void;

  enum class state_t { uninitialized, value, error };

  state_t state_ = state_t::uninitialized;
  std::exception_ptr error_;

  void emplace_value() noexcept {
    state_ = state_t::value;
  }

  void emplace_error(std::exception_ptr e) noexcept {
    error_ = std::move(e);
    state_ = state_t::error;
  }

  void get() {
    if (state_ == state_t::error) {
      auto e = std::move(error_);
      state_ = state_t::uninitialized;
      std::rethrow_exception(std::move(e));
    }
  }
};

// ---------------------------------------------------------------------------
//  Detail: promise base with conditional return_value / return_void
//  The C++ standard forbids a promise from declaring both. We use
//  separate base class specialisations so only one exists per T.
// ---------------------------------------------------------------------------

template<typename T>
struct promise_return_base : promise_result_storage<T> {
  using promise_result_storage<T>::emplace_value;

  template<typename U>
    requires std::constructible_from<T, U&&>
  void return_value(U&& v) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
    emplace_value(std::forward<U>(v));
  }
};

template<>
struct promise_return_base<void> : promise_result_storage<void> {
  using promise_result_storage<void>::emplace_value;

  void return_void() noexcept {
    emplace_value();
  }
};

} // namespace detail

// ---------------------------------------------------------------------------
//  task<T>
// ---------------------------------------------------------------------------

template<typename T = void>
class task {
public:
  using value_type = T;

  // -- promise_type -------------------------------------------------------

  class promise_type final : public detail::promise_return_base<T> {
  public:
    using storage_type = detail::promise_result_storage<T>;

    auto get_return_object() noexcept -> task {
      return task(std::coroutine_handle<promise_type>::from_promise(*this));
    }

    auto initial_suspend() noexcept -> std::suspend_always {
      return {};
    }

    auto final_suspend() noexcept {
      struct final_awaiter {
        bool await_ready() const noexcept {
          return false;
        }

        auto await_suspend(std::coroutine_handle<promise_type> h) noexcept
            -> std::coroutine_handle<> {
          auto& p = h.promise();
          if (p.notify_fn_) {
            p.notify_fn_(p.notify_ctx_, p);
            return std::noop_coroutine();
          }
          if (p.continuation_) {
            return p.continuation_;
          }
          return std::noop_coroutine();
        }

        void await_resume() const noexcept {}
      };
      return final_awaiter{};
    }

    void unhandled_exception() noexcept {
      this->emplace_error(std::current_exception());
    }

    auto result() -> T {
      return this->get();
    }

    void set_continuation(std::coroutine_handle<> h) noexcept {
      continuation_ = h;
    }

    using notify_fn_t = void(void* ctx, promise_type& p) noexcept;

    void set_notify(void* ctx, notify_fn_t* fn) noexcept {
      notify_ctx_ = ctx;
      notify_fn_ = fn;
    }

  private:
    std::coroutine_handle<> continuation_{};
    void* notify_ctx_{nullptr};
    notify_fn_t* notify_fn_{nullptr};
  };

  // -- Constructors / destructor ------------------------------------------

  task() noexcept = default;

  explicit task(std::coroutine_handle<promise_type> h) noexcept : coro_(h) {}

  task(task&& other) noexcept : coro_(std::exchange(other.coro_, {})) {}

  auto operator=(task&& other) noexcept -> task& {
    if (this != &other) {
      if (coro_) {
        coro_.destroy();
      }
      coro_ = std::exchange(other.coro_, {});
    }
    return *this;
  }

  task(const task&) = delete;
  auto operator=(const task&) -> task& = delete;

  ~task() {
    if (coro_) {
      coro_.destroy();
    }
  }

  // -- Awaitable (co_await task<T>) ---------------------------------------

  auto operator co_await() && noexcept {
    struct awaiter {
      std::coroutine_handle<promise_type> handle;

      auto await_ready() const noexcept -> bool {
        return false;
      }

      auto await_suspend(std::coroutine_handle<> awaiting) const noexcept
          -> std::coroutine_handle<> {
        handle.promise().set_continuation(awaiting);
        return handle;
      }

      auto await_resume() -> T {
        return handle.promise().result();
      }
    };
    return awaiter{std::exchange(coro_, {})};
  }

  // -- Sender (connect) ---------------------------------------------------

  template<typename R>
    requires(std::is_void_v<T> ? receiver_of<R> : receiver_of<R, T>)
  auto connect(R&& r) && {
    return op_state<std::remove_cvref_t<R>>(std::exchange(coro_, {}),
                                            std::forward<R>(r));
  }

  // -- Utility ------------------------------------------------------------

  auto handle() const noexcept -> std::coroutine_handle<promise_type> {
    return coro_;
  }

  explicit operator bool() const noexcept {
    return coro_ != nullptr;
  }

private:
  std::coroutine_handle<promise_type> coro_{};

  // -- Operation state for the sender/receiver path -----------------------

  template<typename R>
  struct op_state final {
    std::coroutine_handle<promise_type> handle_;
    R receiver_;

    op_state(std::coroutine_handle<promise_type> h, R&& r)
      : handle_(h), receiver_(std::forward<R>(r)) {}

    static void notify(void* ctx, promise_type& p) noexcept {
      auto& self = *static_cast<op_state*>(ctx);
      if (p.state_ == detail::promise_result_storage<T>::state_t::error) {
        coro::set_error(std::move(self.receiver_), std::move(p.error_));
      } else {
        if constexpr (std::is_void_v<T>) {
          coro::set_value(std::move(self.receiver_));
        } else {
          coro::set_value(std::move(self.receiver_), std::move(p.value_));
        }
      }
    }

    void start() noexcept {
      auto& promise = handle_.promise();
      promise.set_notify(static_cast<void*>(this), &notify);
      handle_.resume();
    }
  };
};

} // namespace coro
