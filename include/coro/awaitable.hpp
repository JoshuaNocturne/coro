#pragma once

/// @file awaitable.hpp
/// Awaitable traits and utilities for C++20 coroutines.
///
/// Provides the machinery to inspect and adapt awaitable types,
/// forming the bridge between the coroutine language feature and
/// the Sender/Receiver abstraction layer.

#include <concepts>
#include <type_traits>
#include <utility>

#include <coro/config.hpp>

namespace coro {

// ---------------------------------------------------------------------------
//  awaitable_traits — inspect what co_await produces
// ---------------------------------------------------------------------------

template<typename T>
struct awaitable_traits;

// Primary: if T has operator co_await, use its result.
template<typename T>
  requires requires(T&& t) { static_cast<T&&>(t).operator co_await(); }
struct awaitable_traits<T> {
  using awaiter_t =
      decltype(static_cast<T&&>(std::declval<T&>()).operator co_await());
  using await_result_t =
      decltype(static_cast<awaiter_t>(std::declval<awaiter_t&>())
                   .await_resume());
};

// ADL operator co_await
template<typename T>
  requires(!requires(T&& t) { static_cast<T&&>(t).operator co_await(); }) &&
          requires(T&& t) { operator co_await(static_cast<T&&>(t)); }
struct awaitable_traits<T> {
  using awaiter_t =
      decltype(operator co_await(static_cast<T&&>(std::declval<T&>())));
  using await_result_t =
      decltype(static_cast<awaiter_t>(std::declval<awaiter_t&>())
                   .await_resume());
};

// Directly awaitable (has await_ready/suspend/resume)
template<typename T>
  requires(!requires(T&& t) { static_cast<T&&>(t).operator co_await(); }) &&
          (!requires(T&& t) { operator co_await(static_cast<T&&>(t)); }) &&
          requires(T&& t) {
            { static_cast<T&&>(t).await_ready() } -> std::convertible_to<bool>;
            static_cast<T&&>(t).await_suspend(
                std::declval<std::coroutine_handle<>>());
            static_cast<T&&>(t).await_resume();
          }
struct awaitable_traits<T> {
  using awaiter_t = T&&;
  using await_result_t =
      decltype(static_cast<T&&>(std::declval<T&>()).await_resume());
};

template<typename T>
using await_result_t =
    typename awaitable_traits<std::remove_cvref_t<T>>::await_result_t;

// ---------------------------------------------------------------------------
//  is_awaitable — type trait
// ---------------------------------------------------------------------------

template<typename T, typename = void>
struct is_awaitable : std::false_type {};

template<typename T>
struct is_awaitable<T,
                    std::void_t<typename awaitable_traits<T>::await_result_t>>
  : std::true_type {};

template<typename T>
inline constexpr bool is_awaitable_v = is_awaitable<T>::value;

// ---------------------------------------------------------------------------
//  as_awaitable — adapt a type for co_await usage
// ---------------------------------------------------------------------------

namespace detail {

/// Identity wrapper: if T is already directly awaitable, use it as-is.
template<typename T>
struct as_awaitable_wrapper {
  T value;

  auto operator co_await() && -> decltype(auto) {
    return static_cast<T&&>(value);
  }
};

} // namespace detail

/// Customisation point: convert a value into something that can be co_awaited.
/// Default implementation passes through awaitable types unchanged.
template<typename T>
  requires is_awaitable_v<T>
auto as_awaitable(T&& value) -> decltype(auto) {
  return static_cast<T&&>(value);
}

// ---------------------------------------------------------------------------
//  suspend_never / suspend_always re-exports (for convenience)
// ---------------------------------------------------------------------------

using std::suspend_always;
using std::suspend_never;

} // namespace coro
