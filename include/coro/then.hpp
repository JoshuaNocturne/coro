#pragma once

/// @file then.hpp
/// then — apply a function to a sender's result, producing a new sender.

#include <exception>
#include <functional>
#include <type_traits>
#include <utility>

#include <coro/receiver.hpp>
#include <coro/sender.hpp>

namespace coro {

// ---------------------------------------------------------------------------
//  then_sender
// ---------------------------------------------------------------------------

namespace detail {

template<typename F, typename T>
struct then_result {
  using type = std::invoke_result_t<F, T>;
};

template<typename F>
struct then_result<F, void> {
  using type = std::invoke_result_t<F>;
};

/// Fix a then_receiver's parent_ pointer after its owning op_state has
/// been moved.  Walks into the inner op_state: if it has a public
/// receiver_ member that has a parent_ member, fix it.  Then recurse
/// into the inner op_state's own inner_op_ (for chained then).
template<typename Op, typename Parent>
void fix_then_parent(Op& op, Parent* p) {
  op.receiver_.parent_ = p;
}

/// Overload for then_sender::op_state (which has inner_op_).
/// Recurse: the inner then_receiver's parent should point to this
/// inner op_state.
template<typename Op, typename Parent>
  requires requires(Op& o) { o.inner_op_; }
void fix_then_parent(Op& op, Parent* /*unused*/) {
  fix_then_parent(op.inner_op_, &op);
}

} // namespace detail

template<typename S, typename F>
class then_sender {
  S sender_;
  F func_;

public:
  using input_value_t = sender_value_t<S>;

  // For void input, F must be invocable with no args; otherwise with
  // input_value_t
  using value_type = typename detail::then_result<F, input_value_t>::type;

  then_sender(S&& s, F f) : sender_(static_cast<S&&>(s)), func_(std::move(f)) {}

  // -- connect -----------------------------------------------------------

  template<typename R>
    requires(std::is_void_v<value_type> ? receiver_of<R>
                                        : receiver_of<R, value_type>)
  class op_state {
  public:
    struct then_receiver {
      op_state* parent_;

      // Non-void input
      template<typename V>
      void set_value(V&& v) {
        try {
          if constexpr (std::is_void_v<value_type>) {
            std::invoke(parent_->func_, static_cast<V&&>(v));
            coro::set_value(std::move(parent_->outer_receiver_));
          } else {
            auto result = std::invoke(parent_->func_, static_cast<V&&>(v));
            coro::set_value(std::move(parent_->outer_receiver_),
                            std::move(result));
          }
        } catch (...) {
          coro::set_error(std::move(parent_->outer_receiver_),
                          std::current_exception());
        }
      }

      // Void input
      void set_value()
        requires std::is_void_v<input_value_t>
      {
        try {
          if constexpr (std::is_void_v<value_type>) {
            std::invoke(parent_->func_);
            coro::set_value(std::move(parent_->outer_receiver_));
          } else {
            auto result = std::invoke(parent_->func_);
            coro::set_value(std::move(parent_->outer_receiver_),
                            std::move(result));
          }
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

    using inner_op_t = decltype(coro::connect(std::declval<S>(),
                                              std::declval<then_receiver>()));

    F func_;
    R outer_receiver_;
    inner_op_t inner_op_;

    op_state(S s, F f, R&& r)
      : func_(std::move(f)),
        outer_receiver_(std::forward<R>(r)),
        inner_op_(coro::connect(std::move(s), then_receiver{this})) {}

    void start() noexcept {
      // After a potential move of this op_state, the then_receiver inside
      // inner_op_ may still hold a dangling parent_ pointer.  Fix it.
      detail::fix_then_parent(inner_op_, this);
      coro::start(inner_op_);
    }
  };

  template<typename R>
    requires(std::is_void_v<value_type> ? receiver_of<R>
                                        : receiver_of<R, value_type>)
  auto connect(R&& r) && {
    return op_state<std::remove_cvref_t<R>>(
        std::move(sender_), std::move(func_), std::forward<R>(r));
  }
};

// ---------------------------------------------------------------------------
//  then() free function + pipe support
// ---------------------------------------------------------------------------

template<typename F>
struct then_fn {
  F func;

  template<typename S>
  friend auto operator|(S&& s, then_fn&& self) {
    return then_sender<std::remove_cvref_t<S>, F>(static_cast<S&&>(s),
                                                  std::move(self.func));
  }
};

template<typename F>
auto then(F&& f) {
  return then_fn<std::remove_cvref_t<F>>{static_cast<F&&>(f)};
}

} // namespace coro
