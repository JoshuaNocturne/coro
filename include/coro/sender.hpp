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

#include <concepts>
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

} // namespace coro
