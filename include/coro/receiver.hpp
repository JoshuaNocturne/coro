#pragma once

/// @file receiver.hpp
/// Receiver concept and completion-signature machinery.
///
/// Receivers are the "consumer" side of the Sender/Receiver protocol.
/// A receiver accepts one of three completion signals:
///   - set_value(receiver, args...)   — successful completion
///   - set_error(receiver, error)     — failure completion
///   - set_stopped(receiver)          — cancellation
///
/// Exactly one of these is called, and the receiver is then done.

#include <concepts>
#include <exception>
#include <type_traits>
#include <utility>

#include <coro/config.hpp>

namespace coro {

// ---------------------------------------------------------------------------
//  Completion signatures — describe what a sender can send
// ---------------------------------------------------------------------------

/// A completion signature is a function type whose return type is void
/// and whose parameters describe the data channelled to the receiver.
/// Examples:  void()   void(int)   void(std::exception_ptr)
template<typename Sig>
concept completion_signature = std::is_function_v<std::remove_cvref_t<Sig>>;

// ---------------------------------------------------------------------------
//  set_value customisation point
// ---------------------------------------------------------------------------

namespace cpos {

struct set_value_t {
  template<typename R, typename... Args>
    requires requires(R&& r, Args&&... args) {
      static_cast<R&&>(r).set_value(static_cast<Args&&>(args)...);
    }
  void operator()(R&& r, Args&&... args) const noexcept(
      noexcept(std::forward<R>(r).set_value(std::forward<Args>(args)...))) {
    std::forward<R>(r).set_value(std::forward<Args>(args)...);
  }
};

} // namespace cpos

inline constexpr cpos::set_value_t set_value{};

// ---------------------------------------------------------------------------
//  set_error customisation point
// ---------------------------------------------------------------------------

namespace cpos {

struct set_error_t {
  template<typename R, typename E>
    requires requires(R&& r, E&& e) {
      static_cast<R&&>(r).set_error(static_cast<E&&>(e));
    }
  void operator()(R&& r, E&& e) const
      noexcept(noexcept(std::forward<R>(r).set_error(std::forward<E>(e)))) {
    std::forward<R>(r).set_error(std::forward<E>(e));
  }
};

} // namespace cpos

inline constexpr cpos::set_error_t set_error{};

// ---------------------------------------------------------------------------
//  set_stopped customisation point
// ---------------------------------------------------------------------------

namespace cpos {

struct set_stopped_t {
  template<typename R>
    requires requires(R&& r) { static_cast<R&&>(r).set_stopped(); }
  void operator()(R&& r) const
      noexcept(noexcept(std::forward<R>(r).set_stopped())) {
    std::forward<R>(r).set_stopped();
  }
};

} // namespace cpos

inline constexpr cpos::set_stopped_t set_stopped{};

// ---------------------------------------------------------------------------
//  Receiver concepts
// ---------------------------------------------------------------------------

/// A receiver can accept set_error and set_stopped (must be noexcept).
/// Specific value-shapes are handled by receiver_of<T...>.
template<typename R>
concept receiver = requires(R&& r, std::exception_ptr e) {
  { set_error(static_cast<R&&>(r), std::move(e)) } noexcept;
  { set_stopped(static_cast<R&&>(r)) } noexcept;
};

/// A receiver that can accept a specific set_value shape.
template<typename R, typename... Args>
concept receiver_of = receiver<R> && requires(R&& r, Args&&... args) {
  set_value(static_cast<R&&>(r), static_cast<Args&&>(args)...);
};

} // namespace coro
