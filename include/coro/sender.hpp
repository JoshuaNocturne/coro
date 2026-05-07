#pragma once

/// @file sender.hpp
/// Sender concept and the connect / start customisation points.
///
/// Senders are lazy descriptions of asynchronous work. They are composed
/// through algorithms and eventually connected to a receiver via `connect`,
/// producing an operation_state that is `start`ed.
///
/// This file provides:
///   - sender concept
///   - connect(sender, receiver) -> operation_state
///   - start(operation_state&)    — begins the operation
///   - operation_state concept

#include <atomic>
#include <concepts>
#include <coroutine>
#include <exception>
#include <optional>
#include <type_traits>
#include <utility>

#include <coro/receiver.hpp>

namespace coro {

// ---------------------------------------------------------------------------
//  operation_state concept and start CPO
// ---------------------------------------------------------------------------

namespace cpos {

struct start_t {
  template<typename O>
    requires requires(O& o) { o.start(); }
  void operator()(O& o) const noexcept(noexcept(o.start())) {
    o.start();
  }
};

} // namespace cpos

inline constexpr cpos::start_t start{};

template<typename O>
concept operation_state = requires(O& o) {
  { start(o) } noexcept;
};

// ---------------------------------------------------------------------------
//  connect CPO
// ---------------------------------------------------------------------------

namespace cpos {

struct connect_t {
  template<typename S, typename R>
    requires requires(S&& s, R&& r) {
      static_cast<S&&>(s).connect(static_cast<R&&>(r));
    }
  auto operator()(S&& s, R&& r) const
      noexcept(noexcept(std::forward<S>(s).connect(std::forward<R>(r))))
          -> decltype(std::forward<S>(s).connect(std::forward<R>(r))) {
    return std::forward<S>(s).connect(std::forward<R>(r));
  }
};

} // namespace cpos

inline constexpr cpos::connect_t connect{};

// ---------------------------------------------------------------------------
//  Sender concept
// ---------------------------------------------------------------------------

/// A sender describes async work and can be connected to a receiver.
/// It must be move-constructible and expose a `value_type` alias.
template<typename S>
concept sender = std::move_constructible<std::remove_cvref_t<S>> &&
                 requires { typename std::remove_cvref_t<S>::value_type; };

/// A sender that can be connected to a specific receiver type.
template<typename S, typename R>
concept sender_to = sender<S> && receiver<R> && requires(S&& s, R&& r) {
  { connect(static_cast<S&&>(s), static_cast<R&&>(r)) } -> operation_state;
};

// ---------------------------------------------------------------------------
//  Helper: deduce the value type of a sender
// ---------------------------------------------------------------------------

namespace detail {

template<typename Sig>
struct sig_value_impl;

template<typename... Args>
struct sig_value_impl<void(Args...)> {
  using type = std::conditional_t<
      sizeof...(Args) == 0, void,
      std::conditional_t<sizeof...(Args) == 1,
                         std::tuple_element_t<0, std::tuple<Args...>>,
                         std::tuple<Args...>>>;
};

} // namespace detail

/// Extracts the value type from a sender's value completion signature.
/// Specialise for your sender types, or provide `value_type` nested alias.
template<typename S>
struct sender_traits;

template<typename S>
  requires requires { typename std::remove_cvref_t<S>::value_type; }
struct sender_traits<S> {
  using value_type = typename std::remove_cvref_t<S>::value_type;
};

template<typename S>
using sender_value_t = typename sender_traits<S>::value_type;

// ---------------------------------------------------------------------------
//  sender_awaiter — adapt any sender for co_await usage (P2300 style)
//
//  "Many senders can be trivially made awaitable" — P2300.
//  This provides a generic bridge: connect(sender, receiver) + start(op)
//  wrapped in an awaiter that works inside any coroutine.
//
//  Design constraint (P2583R0): the receiver must never call .resume()
//  inside await_suspend, as this leads to double-resume UB when the
//  await_suspend return value also resumes the coroutine.
//
//  Race-free protocol using a three-state atomic:
//    0 (initial)   → await_suspend is still running
//    1 (suspended) → await_suspend has returned noop_coroutine()
//    2 (completed) → receiver has invoked a completion function
//
//    - await_suspend CAS 0→1.  On failure (already 2) → symmetric transfer.
//    - receiver     CAS 0→2.  On failure (already 1) → resume continuation.
//  Exactly one path resumes the coroutine, with no re-check race window.
// ---------------------------------------------------------------------------

namespace detail {

// States for the sender_await_state atomic state machine.
inline constexpr int state_initial = 0;
inline constexpr int state_suspended = 1;
inline constexpr int state_completed = 2;

/// Shared state between sender_awaiter and sender_await_receiver.
/// The awaiter owns the state; the receiver holds a pointer to it.
///
/// Uses a three-state atomic to guarantee exactly one resume path:
///   0 (initial)   → await_suspend is still running
///   1 (suspended) → await_suspend has returned noop_coroutine()
///   2 (completed) → receiver has invoked a completion function
///
/// Race-free protocol:
///   - await_suspend CAS 0→1.  On failure (already 2) return awaiting handle.
///   - receiver CAS 0→2.  On failure (already 1) resume continuation.
template<typename T>
struct sender_await_state {
  std::coroutine_handle<> continuation_{};
  std::optional<T> value_;
  std::exception_ptr error_;
  bool stopped_ = false;
  std::atomic<int> state_{state_initial};
};

template<>
struct sender_await_state<void> {
  std::coroutine_handle<> continuation_{};
  std::exception_ptr error_;
  bool stopped_ = false;
  std::atomic<int> state_{state_initial};
};

/// Shared CAS completion protocol: transition 0→2 and resume if already
/// suspended.  Extracted to eliminate duplication between the void and
/// non-void receiver specialisations.
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

template<typename T>
struct sender_await_receiver {
  sender_await_state<T>* state_;

  template<typename V>
  void set_value(V&& v) {
    state_->value_.emplace(std::forward<V>(v));
    sender_await_complete(state_);
  }

  void set_error(std::exception_ptr e) noexcept {
    state_->error_ = std::move(e);
    sender_await_complete(state_);
  }

  void set_stopped() noexcept {
    state_->stopped_ = true;
    sender_await_complete(state_);
  }
};

template<>
struct sender_await_receiver<void> {
  sender_await_state<void>* state_;

  void set_value() {
    sender_await_complete(state_);
  }

  void set_error(std::exception_ptr e) noexcept {
    state_->error_ = std::move(e);
    sender_await_complete(state_);
  }

  void set_stopped() noexcept {
    state_->stopped_ = true;
    sender_await_complete(state_);
  }
};

/// Generic awaiter that wraps any sender via connect + start.
template<typename S>
  requires sender<std::remove_cvref_t<S>>
class sender_awaiter {
  using sender_decay_t = std::remove_cvref_t<S>;
  using value_t = sender_value_t<sender_decay_t>;
  using state_t = sender_await_state<value_t>;
  using receiver_t = sender_await_receiver<value_t>;

  sender_decay_t sender_;
  state_t state_{};

  using op_t = decltype(coro::connect(std::declval<sender_decay_t>(),
                                      std::declval<receiver_t>()));
  std::optional<op_t> op_;

public:
  explicit sender_awaiter(S&& s) : sender_(std::forward<S>(s)) {}

  auto await_ready() const noexcept -> bool {
    return false;
  }

  auto await_suspend(std::coroutine_handle<> awaiting)
      -> std::coroutine_handle<> {
    // Set continuation first so the receiver can find it.
    state_.continuation_ = awaiting;
    op_.emplace(coro::connect(std::move(sender_), receiver_t{&state_}));
    coro::start(*op_);

    // Atomically commit to suspending (0→1).
    // If the receiver already completed (CAS fails, state == 2),
    // the sender finished — use symmetric transfer.
    int expected = state_initial;
    if (!state_.state_.compare_exchange_strong(expected, state_suspended,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
      return awaiting;
    }

    return std::noop_coroutine();
  }

  auto await_resume() -> value_t {
    if (state_.error_) {
      std::rethrow_exception(std::move(state_.error_));
    }
    if (state_.stopped_) {
      throw std::runtime_error("sender stopped");
    }
    if constexpr (!std::is_void_v<value_t>) {
      return std::move(*state_.value_);
    }
  }
};

} // namespace detail

/// ADL operator co_await: makes any sender directly awaitable in coroutines.
/// Excluded: types that already have their own operator co_await (e.g.
/// task<T>).
template<typename S>
  requires sender<std::remove_cvref_t<S>> && (!requires {
             std::declval<std::remove_cvref_t<S>>().operator co_await();
           })
auto operator co_await(S&& s) {
  return detail::sender_awaiter<S>(std::forward<S>(s));
}

} // namespace coro
