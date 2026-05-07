#pragma once

/// @file let_value.hpp
/// let_value — apply a function that returns a sender to a sender's result,
/// producing a flattened sender.  The function's returned sender is
/// connected and started, and its completion is forwarded to the outer
/// receiver.  This is the async equivalent of monadic bind (>>=).

#include <exception>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

#include <coro/receiver.hpp>
#include <coro/sender.hpp>

namespace coro {

// ---------------------------------------------------------------------------
//  let_value_sender — forward declaration
// ---------------------------------------------------------------------------

template<typename S, typename F>
class let_value_sender;

// ---------------------------------------------------------------------------
//  Detail: value type trait
// ---------------------------------------------------------------------------

namespace detail {

template<typename F, typename T>
struct let_value_result {
  using second_sender_t = std::invoke_result_t<F, T>;
  using type = sender_value_t<second_sender_t>;
};

template<typename F>
struct let_value_result<F, void> {
  using second_sender_t = std::invoke_result_t<F>;
  using type = sender_value_t<second_sender_t>;
};

// ---------------------------------------------------------------------------
//  Detail: fix_then_parent overload for let_value op_state
// ---------------------------------------------------------------------------

template<typename Op, typename Parent>
  requires requires(Op& o) { o.phase1_op_; }
void fix_then_parent(Op& op, Parent* /*unused*/) {
  fix_then_parent(op.phase1_op_, &op);
}

// ---------------------------------------------------------------------------
//  Detail: op_state for non-void input
// ---------------------------------------------------------------------------

template<typename S, typename F, typename R, typename InputValueT>
class let_value_op_state;

template<typename S, typename F, typename R, typename InputValueT>
class let_value_op_state {
public:
  using input_value_t = InputValueT;
  using value_type = typename let_value_result<F, input_value_t>::type;
  using second_sender_t =
      typename let_value_result<F, input_value_t>::second_sender_t;

  // --- Phase 2 receiver: forwards inner sender completion to outer ---

  struct phase2_receiver {
    let_value_op_state* parent_;

    // Non-void output
    template<typename V>
    void set_value(V&& v)
      requires(!std::is_void_v<value_type>)
    {
      coro::set_value(std::move(parent_->outer_receiver_), std::forward<V>(v));
    }

    // Void output
    void set_value()
      requires std::is_void_v<value_type>
    {
      coro::set_value(std::move(parent_->outer_receiver_));
    }

    void set_error(std::exception_ptr e) noexcept {
      coro::set_error(std::move(parent_->outer_receiver_), std::move(e));
    }

    void set_stopped() noexcept {
      coro::set_stopped(std::move(parent_->outer_receiver_));
    }
  };

  // --- Phase 1 receiver: invokes func, connects phase 2 ---

  struct let_value_receiver {
    let_value_op_state* parent_;

    // Non-void input
    template<typename V>
    void set_value(V&& v) {
      try {
        auto sender2 = std::invoke(parent_->func_, std::forward<V>(v));
        parent_->phase2_op_.emplace(
            coro::connect(std::move(sender2), phase2_receiver{parent_}));
        coro::start(*parent_->phase2_op_);
      } catch (...) {
        coro::set_error(std::move(parent_->outer_receiver_),
                        std::current_exception());
      }
    }

    void set_error(std::exception_ptr e) noexcept {
      coro::set_error(std::move(parent_->outer_receiver_), std::move(e));
    }

    void set_stopped() noexcept {
      coro::set_stopped(std::move(parent_->outer_receiver_));
    }
  };

  // --- Type computations ---

  using phase1_op_t = decltype(coro::connect(
      std::declval<S>(), std::declval<let_value_receiver>()));

  using phase2_op_t = decltype(coro::connect(std::declval<second_sender_t>(),
                                             std::declval<phase2_receiver>()));

  // --- Data members ---

  F func_;
  R outer_receiver_;
  phase1_op_t phase1_op_;
  std::optional<phase2_op_t> phase2_op_{std::nullopt};

  // --- Constructor ---

  let_value_op_state(S s, F f, R&& r)
    : func_(std::move(f)),
      outer_receiver_(std::forward<R>(r)),
      phase1_op_(coro::connect(std::move(s), let_value_receiver{this})) {}

  // --- Start ---

  void start() noexcept {
    // Unqualified call enables ADL to find overloads from other headers
    using detail::fix_then_parent;
    fix_then_parent(phase1_op_, this);
    coro::start(phase1_op_);
  }
};

// ---------------------------------------------------------------------------
//  Detail: op_state for void input
// ---------------------------------------------------------------------------

template<typename S, typename F, typename R>
class let_value_op_state<S, F, R, void> {
public:
  using input_value_t = void;
  using value_type = typename let_value_result<F, void>::type;
  using second_sender_t = typename let_value_result<F, void>::second_sender_t;

  // --- Phase 2 receiver: forwards inner sender completion to outer ---

  struct phase2_receiver {
    let_value_op_state* parent_;

    // Non-void output
    template<typename V>
    void set_value(V&& v)
      requires(!std::is_void_v<value_type>)
    {
      coro::set_value(std::move(parent_->outer_receiver_), std::forward<V>(v));
    }

    // Void output
    void set_value()
      requires std::is_void_v<value_type>
    {
      coro::set_value(std::move(parent_->outer_receiver_));
    }

    void set_error(std::exception_ptr e) noexcept {
      coro::set_error(std::move(parent_->outer_receiver_), std::move(e));
    }

    void set_stopped() noexcept {
      coro::set_stopped(std::move(parent_->outer_receiver_));
    }
  };

  // --- Phase 1 receiver: invokes func (no args), connects phase 2 ---

  struct let_value_receiver {
    let_value_op_state* parent_;

    void set_value() {
      try {
        auto sender2 = std::invoke(parent_->func_);
        parent_->phase2_op_.emplace(
            coro::connect(std::move(sender2), phase2_receiver{parent_}));
        coro::start(*parent_->phase2_op_);
      } catch (...) {
        coro::set_error(std::move(parent_->outer_receiver_),
                        std::current_exception());
      }
    }

    void set_error(std::exception_ptr e) noexcept {
      coro::set_error(std::move(parent_->outer_receiver_), std::move(e));
    }

    void set_stopped() noexcept {
      coro::set_stopped(std::move(parent_->outer_receiver_));
    }
  };

  // --- Type computations ---

  using phase1_op_t = decltype(coro::connect(
      std::declval<S>(), std::declval<let_value_receiver>()));

  using phase2_op_t = decltype(coro::connect(std::declval<second_sender_t>(),
                                             std::declval<phase2_receiver>()));

  // --- Data members ---

  F func_;
  R outer_receiver_;
  phase1_op_t phase1_op_;
  std::optional<phase2_op_t> phase2_op_{std::nullopt};

  // --- Constructor ---

  let_value_op_state(S s, F f, R&& r)
    : func_(std::move(f)),
      outer_receiver_(std::forward<R>(r)),
      phase1_op_(coro::connect(std::move(s), let_value_receiver{this})) {}

  // --- Start ---

  void start() noexcept {
    using detail::fix_then_parent;
    fix_then_parent(phase1_op_, this);
    coro::start(phase1_op_);
  }
};

} // namespace detail

// ---------------------------------------------------------------------------
//  let_value_sender
// ---------------------------------------------------------------------------

template<typename S, typename F>
class let_value_sender {
  S sender_;
  F func_;

public:
  using input_value_t = sender_value_t<S>;
  using value_type = typename detail::let_value_result<F, input_value_t>::type;

  let_value_sender(S&& s, F f)
    : sender_(std::forward<S>(s)), func_(std::move(f)) {}

  // -- connect -----------------------------------------------------------

  template<typename R>
    requires(std::is_void_v<value_type> ? receiver_of<R>
                                        : receiver_of<R, value_type>)
  auto connect(R&& r) && {
    return detail::let_value_op_state<S, F, std::remove_cvref_t<R>,
                                      input_value_t>(
        std::move(sender_), std::move(func_), std::forward<R>(r));
  }
};

// ---------------------------------------------------------------------------
//  let_value() free function + pipe support
// ---------------------------------------------------------------------------

template<typename F>
struct let_value_fn {
  F func;

  template<typename S>
  friend auto operator|(S&& s, let_value_fn&& self) {
    return let_value_sender<std::remove_cvref_t<S>, F>(std::forward<S>(s),
                                                       std::move(self.func));
  }
};

template<typename F>
auto let_value(F&& f) {
  return let_value_fn<std::remove_cvref_t<F>>{std::forward<F>(f)};
}

} // namespace coro
