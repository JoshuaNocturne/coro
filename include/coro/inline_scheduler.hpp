#pragma once

/// @file inline_scheduler.hpp
/// inline_scheduler — completes synchronously on the current thread.

#include <type_traits>
#include <utility>

#include <coro/receiver.hpp>
#include <coro/sender.hpp>

namespace coro {

class inline_scheduler {
public:
  class sender {
  public:
    using value_type = void;

    template<typename R>
      requires receiver_of<R>
    class op_state {
    public:
      R receiver_;

      explicit op_state(R&& r) : receiver_(static_cast<R&&>(r)) {}

      void start() noexcept {
        coro::set_value(std::move(receiver_));
      }
    };

    template<typename R>
      requires receiver_of<R>
    auto connect(R&& r) && {
      return op_state<std::remove_cvref_t<R>>(static_cast<R&&>(r));
    }

    template<typename R>
      requires receiver_of<R>
    auto connect(R&& r) const& {
      return op_state<std::remove_cvref_t<R>>(static_cast<R&&>(r));
    }
  };

  auto schedule() const noexcept -> sender {
    return {};
  }
};

} // namespace coro
