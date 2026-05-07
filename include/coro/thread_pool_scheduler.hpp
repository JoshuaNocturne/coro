#pragma once

/// @file thread_pool_scheduler.hpp
/// thread_pool_scheduler — P2300-style scheduler backed by a fixed-size thread
/// pool.
///
/// Design (P2300 pure-lazy):
///   - Only exposes schedule(), returning a void sender that completes on a
///     worker thread.
///   - No eager execute/run interface. All work is lazily described.
///   - To run a coroutine on the pool, co_await schedule() inside it.

#include <condition_variable>
#include <coroutine>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <coro/receiver.hpp>
#include <coro/sender.hpp>

namespace coro {

class thread_pool_scheduler {
public:
  // -- shared pool state (public so sender/op_state can access) ---------------

  struct pool_state {
    std::mutex mtx;
    std::condition_variable cv;
    std::vector<std::function<void()>> queue;
    bool stopping = false;
    std::vector<std::thread> workers;

    void enqueue(std::function<void()> task) {
      {
        auto lock = std::lock_guard(mtx);
        queue.push_back(std::move(task));
      }
      cv.notify_one();
    }
  };

  // -- construct / destruct --------------------------------------------------

  explicit thread_pool_scheduler(
      std::size_t thread_count = std::thread::hardware_concurrency());

  ~thread_pool_scheduler();

  thread_pool_scheduler(const thread_pool_scheduler&) = delete;
  thread_pool_scheduler& operator=(const thread_pool_scheduler&) = delete;
  thread_pool_scheduler(thread_pool_scheduler&&) = delete;
  thread_pool_scheduler& operator=(thread_pool_scheduler&&) = delete;

  // -- sender returned by schedule() -----------------------------------------

  class sender {
  public:
    using value_type = void;

    template<typename R>
      requires receiver_of<R>
    class op_state {
    public:
      R receiver_;
      std::shared_ptr<pool_state> pool_;

      op_state(R&& r, std::shared_ptr<pool_state> pool)
        : receiver_(std::forward<R>(r)), pool_(std::move(pool)) {}

      void start() noexcept;
    };

    template<typename R>
      requires receiver_of<R>
    auto connect(R&& r) && {
      return op_state<std::remove_cvref_t<R>>(std::forward<R>(r), pool_);
    }

    template<typename R>
      requires receiver_of<R>
    auto connect(R&& r) const& {
      return op_state<std::remove_cvref_t<R>>(std::forward<R>(r), pool_);
    }

    // Custom co_await: guarantees the coroutine resumes on a worker thread.
    auto operator co_await() && noexcept {
      struct awaiter {
        std::shared_ptr<pool_state> pool_;
        std::coroutine_handle<> awaiting_{};

        bool await_ready() const noexcept { return false; }

        auto await_suspend(std::coroutine_handle<> h) noexcept
            -> std::coroutine_handle<> {
          awaiting_ = h;
          pool_->enqueue([this]() { awaiting_.resume(); });
          return std::noop_coroutine();
        }

        void await_resume() const noexcept {}
      };
      return awaiter{std::move(pool_)};
    }

  private:
    friend class thread_pool_scheduler;
    explicit sender(std::shared_ptr<pool_state> pool) noexcept
      : pool_(std::move(pool)) {}
    std::shared_ptr<pool_state> pool_;
  };

  // -- schedule() ------------------------------------------------------------

  auto schedule() const noexcept -> sender {
    return sender(state_);
  }

private:
  std::shared_ptr<pool_state> state_;

  // -- worker thread entry point ---------------------------------------------

  static void worker_loop(pool_state& s) {
    while (true) {
      std::function<void()> task;
      {
        auto lock = std::unique_lock(s.mtx);
        s.cv.wait(lock, [&] { return s.stopping || !s.queue.empty(); });
        if (s.stopping && s.queue.empty()) {
          return;
        }
        task = std::move(s.queue.front());
        s.queue.erase(s.queue.begin());
      }
      task();
    }
  }
};

// -- out-of-line definitions -------------------------------------------------

inline thread_pool_scheduler::thread_pool_scheduler(std::size_t thread_count)
  : state_(std::make_shared<pool_state>()) {
  for (std::size_t i = 0; i < thread_count; ++i) {
    state_->workers.emplace_back([s = state_] { worker_loop(*s); });
  }
}

inline thread_pool_scheduler::~thread_pool_scheduler() {
  {
    auto lock = std::lock_guard(state_->mtx);
    state_->stopping = true;
  }
  state_->cv.notify_all();
  for (auto& w : state_->workers) {
    if (w.joinable()) {
      w.join();
    }
  }
}

template<typename R>
  requires receiver_of<R>
void thread_pool_scheduler::sender::op_state<R>::start() noexcept {
  auto* self = this;
  pool_->enqueue([self]() { coro::set_value(std::move(self->receiver_)); });
}

} // namespace coro
