#pragma once

/// @file sync_wait.hpp
/// sync_wait — block the current thread until a sender completes.
///
/// sync_wait connects a sender to a blocking receiver and starts the
/// operation. This is the bridge between the asynchronous Sender/Receiver
/// world and synchronous code (tests, main(), etc.).
///
/// Usage:
///   auto result = sync_wait(some_task());
///   // result is of type T

#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

#include <coro/receiver.hpp>
#include <coro/sender.hpp>

namespace coro {

// ---------------------------------------------------------------------------
//  sync_wait implementation
// ---------------------------------------------------------------------------

namespace detail {

/// Receiver used by sync_wait. Stores the result and notifies a
/// condition variable.
template<typename T>
class sync_wait_receiver {
public:
  using value_type = T;

  struct shared_state {
    std::mutex mtx;
    std::condition_variable cv;
    std::optional<T> value;
    std::exception_ptr error;
    bool done = false;
  };

  explicit sync_wait_receiver(shared_state* s) noexcept : state_(s) {}

  // set_value
  template<typename... Args>
    requires(sizeof...(Args) <= 1)
  void set_value(Args&&... args) {
    auto lock = std::lock_guard(state_->mtx);
    if constexpr (sizeof...(Args) == 1) {
      state_->value.emplace(std::forward<Args>(args)...);
    } else {
      state_->value.emplace(); // void represented as monostate or similar
    }
    state_->done = true;
    state_->cv.notify_one();
  }

  // set_error
  void set_error(std::exception_ptr e) noexcept {
    auto lock = std::lock_guard(state_->mtx);
    state_->error = std::move(e);
    state_->done = true;
    state_->cv.notify_one();
  }

  // set_stopped
  void set_stopped() noexcept {
    auto lock = std::lock_guard(state_->mtx);
    state_->done = true;
    state_->cv.notify_one();
  }

private:
  shared_state* state_;
};

/// Specialisation for void.
template<>
class sync_wait_receiver<void> {
public:
  using value_type = void;

  struct shared_state {
    std::mutex mtx;
    std::condition_variable cv;
    bool value_set = false;
    std::exception_ptr error;
    bool done = false;
  };

  explicit sync_wait_receiver(shared_state* s) noexcept : state_(s) {}

  void set_value() {
    auto lock = std::lock_guard(state_->mtx);
    state_->value_set = true;
    state_->done = true;
    state_->cv.notify_one();
  }

  void set_error(std::exception_ptr e) noexcept {
    auto lock = std::lock_guard(state_->mtx);
    state_->error = std::move(e);
    state_->done = true;
    state_->cv.notify_one();
  }

  void set_stopped() noexcept {
    auto lock = std::lock_guard(state_->mtx);
    state_->done = true;
    state_->cv.notify_one();
  }

private:
  shared_state* state_;
};

} // namespace detail

/// Block until the sender completes and return its value.
/// Throws any exception produced by the sender.
template<typename S>
  requires sender<S>
auto sync_wait(S&& s) -> sender_value_t<S> {
  using T = sender_value_t<S>;
  using state_t = typename detail::sync_wait_receiver<T>::shared_state;

  state_t st;
  auto recv = detail::sync_wait_receiver<T>(&st);

  auto op = coro::connect(std::forward<S>(s), std::move(recv));
  coro::start(op);

  auto lock = std::unique_lock(st.mtx);
  st.cv.wait(lock, [&] { return st.done; });

  if (st.error) {
    std::rethrow_exception(std::move(st.error));
  }

  if constexpr (!std::is_void_v<T>) {
    return std::move(*st.value);
  }
}

} // namespace coro
