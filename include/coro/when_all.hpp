#pragma once

/// @file when_all.hpp
/// when_all — complete when all input senders have completed.

#include <atomic>
#include <exception>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

#include <coro/receiver.hpp>
#include <coro/sender.hpp>

namespace coro {

// ---------------------------------------------------------------------------
//  when_all_sender — forward declaration for sub_receiver
// ---------------------------------------------------------------------------

template<typename... Senders>
class when_all_sender;

// ---------------------------------------------------------------------------
//  Detail: sub-receiver for each input sender in when_all
// ---------------------------------------------------------------------------

namespace detail {

template<std::size_t Index, typename ValueT, typename OpState>
class when_all_sub_receiver {
public:
  OpState& op_;

  template<typename V>
  void set_value(V&& v) {
    if (op_.error_flag_.load(std::memory_order_acquire)) {
      return;
    }
    std::get<Index>(op_.results_).emplace(std::forward<V>(v));
    if (op_.remaining_.fetch_sub(1) == 1) {
      op_.on_complete();
    }
  }

  void set_value()
    requires std::is_void_v<ValueT>
  {
    if (op_.error_flag_.load(std::memory_order_acquire)) {
      return;
    }
    // For void results, nothing to store
    if (op_.remaining_.fetch_sub(1) == 1) {
      op_.on_complete();
    }
  }

  void set_error(std::exception_ptr e) noexcept {
    bool expected = false;
    if (op_.error_flag_.compare_exchange_strong(expected, true,
                                                std::memory_order_acq_rel)) {
      op_.error_ = std::move(e);
    }
    if (op_.remaining_.fetch_sub(1) == 1) {
      op_.on_complete();
    }
  }

  void set_stopped() noexcept {
    if (op_.remaining_.fetch_sub(1) == 1) {
      op_.on_complete();
    }
  }
};

} // namespace detail

// ---------------------------------------------------------------------------
//  when_all_sender
// ---------------------------------------------------------------------------

template<typename... Senders>
class when_all_sender {
  static constexpr std::size_t N = sizeof...(Senders);

  std::tuple<std::remove_cvref_t<Senders>...> senders_;

public:
  using value_type = std::tuple<sender_value_t<Senders>...>;

  explicit when_all_sender(Senders&&... s)
    : senders_(std::forward<Senders>(s)...) {}

  // -- connect -----------------------------------------------------------

  template<typename R>
    requires receiver_of<R, value_type>
  class op_state {
  public:
    R receiver_;

    // One optional result per sender (skip void types with a placeholder)
    using results_tuple = std::tuple<
        std::conditional_t<std::is_void_v<sender_value_t<Senders>>,
                           bool, // placeholder for void results
                           std::optional<sender_value_t<Senders>>>...>;

    results_tuple results_{};
    std::atomic<std::size_t> remaining_{N};
    std::atomic<bool> error_flag_{false};
    std::exception_ptr error_;

    // Sub-operations: each is connect(sender, sub_receiver)
    using sub_ops_tuple = std::tuple<decltype(coro::connect(
        std::declval<std::remove_cvref_t<Senders>>(),
        std::declval<detail::when_all_sub_receiver<
            0, // placeholder index, actual differs per
               // sender
            sender_value_t<Senders>, op_state>>()))...>;

    // We can't directly form the sub_ops_tuple type because each
    // sub_receiver has a different Index. Instead, construct at runtime.
    // Use a variant-like approach: store raw bytes and construct in-place.

    // For simplicity with variadic templates, we use a different approach:
    // store sub-operations in a std::tuple built with an index sequence.

    template<std::size_t... Is>
    static auto make_sub_ops_type(std::index_sequence<Is...>)
        -> std::tuple<decltype(coro::connect(
            std::declval<std::remove_cvref_t<Senders>>(),
            std::declval<detail::when_all_sub_receiver<
                Is, sender_value_t<Senders>, op_state>>()))...>;

    using sub_ops_storage =
        decltype(make_sub_ops_type(std::make_index_sequence<N>{}));

    sub_ops_storage sub_ops_;

    op_state(R&& r, when_all_sender& sender)
      : receiver_(std::forward<R>(r)),
        sub_ops_(make_sub_ops(sender, std::make_index_sequence<N>{})) {}

    void start() noexcept {
      start_impl(std::make_index_sequence<N>{});
    }

    /// Called when all sub-operations complete.
    void on_complete() {
      if (error_flag_.load(std::memory_order_acquire)) {
        coro::set_error(std::move(receiver_), std::move(error_));
      } else {
        coro::set_value(std::move(receiver_), collect_results());
      }
    }

    auto collect_results() -> value_type {
      return collect_impl(std::make_index_sequence<N>{});
    }

    template<std::size_t... Is>
    auto collect_impl(std::index_sequence<Is...>) -> value_type {
      return value_type(extract_result<Is>()...);
    }

    template<std::size_t I>
    auto extract_result() -> std::tuple_element_t<I, value_type> {
      using elem_t = std::tuple_element_t<I, value_type>;
      if constexpr (std::is_void_v<elem_t>) {
        // void result — nothing to extract
      } else {
        return std::move(*std::get<I>(results_));
      }
    }

  private:
    template<std::size_t... Is>
    void start_impl(std::index_sequence<Is...>) {
      (coro::start(std::get<Is>(sub_ops_)), ...);
    }

    template<std::size_t... Is>
    auto make_sub_ops(when_all_sender& sender, std::index_sequence<Is...>) {
      return std::make_tuple(coro::connect(
          std::get<Is>(std::move(sender.senders_)),
          detail::when_all_sub_receiver<
              Is,
              sender_value_t<std::tuple_element_t<Is, std::tuple<Senders...>>>,
              op_state>{*this})...);
    }
  };

  template<typename R>
    requires receiver_of<R, value_type>
  auto connect(R&& r) && {
    return op_state<std::remove_cvref_t<R>>(std::forward<R>(r), *this);
  }
};

// ---------------------------------------------------------------------------
//  when_all() free function
// ---------------------------------------------------------------------------

template<typename... Senders>
auto when_all(Senders&&... senders) {
  return when_all_sender<Senders...>(std::forward<Senders>(senders)...);
}

} // namespace coro
