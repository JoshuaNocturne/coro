#pragma once

/// @file scheduler.hpp
/// Scheduler concept and the schedule() customisation point.
///
/// A scheduler is a context on which work can be scheduled. It provides
/// a sender via the `schedule()` CPO that completes on that context.

#include <concepts>
#include <utility>

#include <coro/sender.hpp>

namespace coro {

// ---------------------------------------------------------------------------
//  schedule CPO
// ---------------------------------------------------------------------------

namespace cpos {

struct schedule_t {
  template<typename S>
    requires requires(S&& s) { static_cast<S&&>(s).schedule(); }
  auto operator()(S&& s) const
      noexcept(noexcept(std::forward<S>(s).schedule()))
          -> decltype(std::forward<S>(s).schedule()) {
    return std::forward<S>(s).schedule();
  }
};

} // namespace cpos

inline constexpr cpos::schedule_t schedule{};

// ---------------------------------------------------------------------------
//  Scheduler concept
// ---------------------------------------------------------------------------

template<typename S>
concept scheduler = requires(S& s) {
  { schedule(s) } -> sender;
};

} // namespace coro
