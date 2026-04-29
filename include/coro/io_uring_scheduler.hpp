#pragma once

/// @file io_uring_scheduler.hpp
/// io_uring-based scheduler for asynchronous I/O operations.
///
/// io_uring_scheduler wraps a Linux io_uring instance and exposes it as a
/// P2300-style scheduler. The schedule() CPO returns a sender that completes
/// when an io_uring submission queue entry (SQE) has been processed and a
/// completion queue entry (CQE) is available.
///
/// Readiness model: a coroutine/sender is "ready" when its CQE arrives.
/// A dedicated event-loop thread blocks on io_uring_wait_cqe() and dispatches
/// completion callbacks to the corresponding op_state.

#include <atomic>
#include <cerrno>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

#include <coro/receiver.hpp>
#include <coro/sender.hpp>

extern "C" {
#include <linux/io_uring.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
}

namespace coro {

namespace detail {

// -- minimal io_uring syscalls ------------------------------------------------

inline int io_uring_setup(unsigned entries, struct io_uring_params* p) {
  return static_cast<int>(syscall(SYS_io_uring_setup, entries, p));
}

inline int io_uring_enter(int fd, unsigned to_submit, unsigned min_complete,
                          unsigned flags) {
  return static_cast<int>(
      syscall(SYS_io_uring_enter, fd, to_submit, min_complete, flags));
}

// -- minimal io_uring helpers (subset of liburing) ----------------------------

struct io_uring_ring {
  int ring_fd = -1;

  // SQ
  unsigned* sq_kring_mask = nullptr;
  unsigned* sq_head = nullptr;
  unsigned* sq_tail = nullptr;
  unsigned* sq_array = nullptr;
  unsigned sq_ring_sz = 0;
  unsigned sq_entries = 0;
  struct io_uring_sqe* sq_sqes = nullptr;

  // CQ
  unsigned* cq_kring_mask = nullptr;
  unsigned* cq_head = nullptr;
  unsigned* cq_tail = nullptr;
  unsigned cq_ring_sz = 0;
  unsigned cq_entries = 0;
  struct io_uring_cqe* cq_cqes = nullptr;

  // mmap base pointers (for munmap)
  void* sq_ring_base = nullptr;
  void* cq_ring_base = nullptr;
  size_t sqes_mmap_sz = 0;
};

inline void io_uring_ring_init(io_uring_ring& ring, unsigned entries) {
  struct io_uring_params p{};
  int fd = io_uring_setup(entries, &p);
  if (fd < 0) {
    throw std::system_error(errno, std::generic_category(), "io_uring_setup");
  }

  ring.ring_fd = fd;
  ring.sq_entries = p.sq_entries;
  ring.cq_entries = p.cq_entries;

  // mmap SQ ring
  ring.sq_ring_sz = p.sq_off.array + p.sq_entries * sizeof(__u32);
  void* sq_ptr = mmap(nullptr, ring.sq_ring_sz, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
  if (sq_ptr == MAP_FAILED) {
    close(fd);
    throw std::system_error(errno, std::generic_category(), "mmap SQ_RING");
  }
  ring.sq_ring_base = sq_ptr;
  ring.sq_kring_mask = reinterpret_cast<unsigned*>(static_cast<char*>(sq_ptr) +
                                                   p.sq_off.ring_mask);
  ring.sq_head =
      reinterpret_cast<unsigned*>(static_cast<char*>(sq_ptr) + p.sq_off.head);
  ring.sq_tail =
      reinterpret_cast<unsigned*>(static_cast<char*>(sq_ptr) + p.sq_off.tail);
  ring.sq_array =
      reinterpret_cast<unsigned*>(static_cast<char*>(sq_ptr) + p.sq_off.array);

  // mmap SQ entries
  ring.sqes_mmap_sz = p.sq_entries * sizeof(struct io_uring_sqe);
  ring.sq_sqes = reinterpret_cast<struct io_uring_sqe*>(
      mmap(nullptr, ring.sqes_mmap_sz, PROT_READ | PROT_WRITE,
           MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES));
  if (ring.sq_sqes == MAP_FAILED) {
    munmap(sq_ptr, ring.sq_ring_sz);
    close(fd);
    throw std::system_error(errno, std::generic_category(), "mmap SQES");
  }

  // mmap CQ ring
  ring.cq_ring_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
  void* cq_ptr = mmap(nullptr, ring.cq_ring_sz, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_CQ_RING);
  if (cq_ptr == MAP_FAILED) {
    munmap(ring.sq_sqes, ring.sqes_mmap_sz);
    munmap(sq_ptr, ring.sq_ring_sz);
    close(fd);
    throw std::system_error(errno, std::generic_category(), "mmap CQ_RING");
  }
  ring.cq_ring_base = cq_ptr;
  ring.cq_kring_mask = reinterpret_cast<unsigned*>(static_cast<char*>(cq_ptr) +
                                                   p.cq_off.ring_mask);
  ring.cq_head =
      reinterpret_cast<unsigned*>(static_cast<char*>(cq_ptr) + p.cq_off.head);
  ring.cq_tail =
      reinterpret_cast<unsigned*>(static_cast<char*>(cq_ptr) + p.cq_off.tail);
  ring.cq_cqes = reinterpret_cast<struct io_uring_cqe*>(
      static_cast<char*>(cq_ptr) + p.cq_off.cqes);
}

inline void io_uring_ring_exit(io_uring_ring& ring) {
  if (ring.ring_fd >= 0) {
    if (ring.sq_sqes && ring.sqes_mmap_sz) {
      munmap(ring.sq_sqes, ring.sqes_mmap_sz);
    }
    if (ring.sq_ring_base) {
      munmap(ring.sq_ring_base, ring.sq_ring_sz);
    }
    if (ring.cq_ring_base) {
      munmap(ring.cq_ring_base, ring.cq_ring_sz);
    }
    close(ring.ring_fd);
  }
  ring.ring_fd = -1;
}

inline struct io_uring_sqe* io_uring_get_sqe(io_uring_ring& ring) {
  unsigned head = *ring.sq_head;
  unsigned tail = *ring.sq_tail;
  if ((tail - head) <= ring.sq_kring_mask[0]) {
    return &ring.sq_sqes[tail & ring.sq_kring_mask[0]];
  }
  return nullptr;
}

inline void io_uring_sqe_advance(io_uring_ring& ring,
                                 struct io_uring_sqe* sqe) {
  unsigned tail = *ring.sq_tail;
  ring.sq_array[tail & *ring.sq_kring_mask] =
      static_cast<unsigned>(sqe - ring.sq_sqes);
  __atomic_store_n(ring.sq_tail, tail + 1, __ATOMIC_SEQ_CST);
}

inline int io_uring_submit(io_uring_ring& ring) {
  unsigned tail = *ring.sq_tail;
  unsigned head = *ring.sq_head;
  unsigned to_submit = tail - head;
  if (to_submit > 0) {
    return io_uring_enter(ring.ring_fd, to_submit, 0, IORING_ENTER_GETEVENTS);
  }
  return 0;
}

inline struct io_uring_cqe* io_uring_wait_cqe(io_uring_ring& ring) {
  unsigned head = *ring.cq_head;
  unsigned tail = *ring.cq_tail;
  if (tail - head > 0) {
    return &ring.cq_cqes[head & ring.cq_kring_mask[0]];
  }
  // Block waiting for completion
  int ret = io_uring_enter(ring.ring_fd, 0, 1, IORING_ENTER_GETEVENTS);
  if (ret < 0) {
    return nullptr;
  }
  head = *ring.cq_head;
  return &ring.cq_cqes[head & ring.cq_kring_mask[0]];
}

inline void io_uring_cqe_seen(io_uring_ring& ring, struct io_uring_cqe*) {
  __atomic_store_n(ring.cq_head, *ring.cq_head + 1, __ATOMIC_SEQ_CST);
}

inline void io_uring_prep_nop(struct io_uring_sqe* sqe) {
  sqe->opcode = IORING_OP_NOP;
  sqe->flags = 0;
  sqe->ioprio = 0;
  sqe->fd = -1;
  sqe->off = 0;
  sqe->addr = 0;
  sqe->len = 0;
  sqe->rw_flags = 0;
  sqe->user_data = 0;
}

inline void io_uring_prep_read(struct io_uring_sqe* sqe, int fd, void* buf,
                                unsigned nbytes, __u64 offset) {
  sqe->opcode = IORING_OP_READ;
  sqe->flags = 0;
  sqe->ioprio = 0;
  sqe->fd = fd;
  sqe->off = offset;
  sqe->addr = reinterpret_cast<__u64>(buf);
  sqe->len = nbytes;
  sqe->rw_flags = 0;
  sqe->user_data = 0;
}

inline void io_uring_prep_write(struct io_uring_sqe* sqe, int fd,
                                 const void* buf, unsigned nbytes,
                                 __u64 offset) {
  sqe->opcode = IORING_OP_WRITE;
  sqe->flags = 0;
  sqe->ioprio = 0;
  sqe->fd = fd;
  sqe->off = offset;
  sqe->addr = reinterpret_cast<__u64>(buf);
  sqe->len = nbytes;
  sqe->rw_flags = 0;
  sqe->user_data = 0;
}

inline void io_uring_prep_timeout(struct io_uring_sqe* sqe,
                                   struct __kernel_timespec* ts) {
  sqe->opcode = IORING_OP_TIMEOUT;
  sqe->flags = 0;
  sqe->ioprio = 0;
  sqe->fd = -1;
  sqe->off = 1; // count = 1: fire after 1 timeout event
  sqe->addr = reinterpret_cast<__u64>(ts);
  sqe->len = 1;
  sqe->timeout_flags = 0;
  sqe->user_data = 0;
}

} // namespace detail

// ---------------------------------------------------------------------------
//  io_uring_scheduler
// ---------------------------------------------------------------------------

class io_uring_scheduler {
public:
  // -- shared ring state -----------------------------------------------------

  struct ring_state {
    detail::io_uring_ring ring;
    std::mutex submit_mtx;
    std::thread event_loop_thread;
    std::atomic<bool> stopping{false};

    explicit ring_state(unsigned entries) {
      detail::io_uring_ring_init(ring, entries);
    }

    ~ring_state() {
      detail::io_uring_ring_exit(ring);
    }

    void submit_sq() {
      auto lock = std::lock_guard(submit_mtx);
      detail::io_uring_submit(ring);
    }

    void run_event_loop();
  };

  // -- op_state base for type-erased dispatch
  // ----------------------------------

  struct op_state_base {
    virtual ~op_state_base() = default;
    virtual void on_complete(int result) noexcept = 0;
  };

  // -- construct / destruct --------------------------------------------------

  explicit io_uring_scheduler(unsigned entries = 256)
    : state_(std::make_shared<ring_state>(entries)) {
    state_->event_loop_thread =
        std::thread([s = state_] { s->run_event_loop(); });
  }

  ~io_uring_scheduler() {
    state_->stopping.store(true, std::memory_order_release);
    {
      auto lock = std::lock_guard(state_->submit_mtx);
      auto* sqe = detail::io_uring_get_sqe(state_->ring);
      if (sqe) {
        detail::io_uring_prep_nop(sqe);
        sqe->user_data = 0;
        detail::io_uring_sqe_advance(state_->ring, sqe);
        detail::io_uring_submit(state_->ring);
      }
    }
    if (state_->event_loop_thread.joinable()) {
      state_->event_loop_thread.join();
    }
  }

  io_uring_scheduler(const io_uring_scheduler&) = delete;
  io_uring_scheduler& operator=(const io_uring_scheduler&) = delete;
  io_uring_scheduler(io_uring_scheduler&&) = delete;
  io_uring_scheduler& operator=(io_uring_scheduler&&) = delete;

  // -- sender ----------------------------------------------------------------

  class sender {
  public:
    using value_type = void;

    // Heap-allocated op_state: self-deletes in on_complete().
    // Holds a pointer to the receiver stored in op_handle (which outlives
    // the async operation because sync_wait blocks until completion).
    template<typename R>
    class op_state : public op_state_base {
    public:
      R* receiver_;
      std::shared_ptr<ring_state> ring_;

      op_state(R* r, std::shared_ptr<ring_state> ring)
        : receiver_(r), ring_(std::move(ring)) {}

      void submit() noexcept {
        auto* sqe = detail::io_uring_get_sqe(ring_->ring);
        if (!sqe) {
          coro::set_error(std::move(*receiver_),
                          std::make_exception_ptr(std::runtime_error(
                              "io_uring submission queue full")));
          delete this;
          return;
        }
        detail::io_uring_prep_nop(sqe);
        sqe->user_data = reinterpret_cast<uintptr_t>(this);
        detail::io_uring_sqe_advance(ring_->ring, sqe);
        ring_->submit_sq();
      }

      void on_complete(int result) noexcept override {
        if (result < 0) {
          coro::set_error(std::move(*receiver_),
                          std::make_exception_ptr(std::system_error(
                              -result, std::generic_category())));
        } else {
          coro::set_value(std::move(*receiver_));
        }
        delete this;
      }
    };

    // Handle returned by connect(). Satisfies operation_state concept.
    // Keeps the receiver alive and holds a pointer to the ring state.
    // start() heap-allocates the real op_state which self-deletes on
    // completion.
    template<typename R>
    class op_handle {
    public:
      R receiver_;
      std::shared_ptr<ring_state> ring_;

      op_handle(R&& r, std::shared_ptr<ring_state> ring)
        : receiver_(static_cast<R&&>(r)), ring_(std::move(ring)) {}

      void start() noexcept {
        auto* op = new op_state<R>(std::addressof(receiver_), ring_);
        op->submit();
      }
    };

    template<typename R>
      requires receiver_of<R>
    auto connect(R&& r) && {
      return op_handle<std::remove_cvref_t<R>>(static_cast<R&&>(r), ring_);
    }

    template<typename R>
      requires receiver_of<R>
    auto connect(R&& r) const& {
      return op_handle<std::remove_cvref_t<R>>(static_cast<R&&>(r), ring_);
    }

  private:
    friend class io_uring_scheduler;
    explicit sender(std::shared_ptr<ring_state> ring) noexcept
      : ring_(std::move(ring)) {}
    std::shared_ptr<ring_state> ring_;
  };

  // -- schedule() ------------------------------------------------------------

  auto schedule() const noexcept -> sender {
    return sender(state_);
  }

  // -- async_read() ----------------------------------------------------------
  // Returns a sender<std::size_t> that submits an IORING_OP_READ and completes
  // with the number of bytes read, or set_error on failure.

  class read_sender {
  public:
    using value_type = std::size_t;

    template<typename R>
    class op_state : public op_state_base {
    public:
      R* receiver_;
      std::shared_ptr<ring_state> ring_;
      int fd_;
      void* buf_;
      unsigned nbytes_;

      op_state(R* r, std::shared_ptr<ring_state> ring, int fd, void* buf,
               unsigned nbytes)
        : receiver_(r), ring_(std::move(ring)), fd_(fd), buf_(buf),
          nbytes_(nbytes) {}

      void submit() noexcept {
        auto lock = std::lock_guard(ring_->submit_mtx);
        auto* sqe = detail::io_uring_get_sqe(ring_->ring);
        if (!sqe) {
          coro::set_error(std::move(*receiver_),
                          std::make_exception_ptr(std::runtime_error(
                              "io_uring submission queue full")));
          delete this;
          return;
        }
        detail::io_uring_prep_read(sqe, fd_, buf_, nbytes_, 0);
        sqe->user_data = reinterpret_cast<uintptr_t>(this);
        detail::io_uring_sqe_advance(ring_->ring, sqe);
        detail::io_uring_submit(ring_->ring);
      }

      void on_complete(int result) noexcept override {
        if (result < 0) {
          coro::set_error(std::move(*receiver_),
                          std::make_exception_ptr(std::system_error(
                              -result, std::generic_category())));
        } else {
          coro::set_value(std::move(*receiver_),
                          static_cast<std::size_t>(result));
        }
        delete this;
      }
    };

    template<typename R>
    class op_handle {
    public:
      R receiver_;
      std::shared_ptr<ring_state> ring_;
      int fd_;
      void* buf_;
      unsigned nbytes_;

      op_handle(R&& r, std::shared_ptr<ring_state> ring, int fd, void* buf,
                unsigned nbytes)
        : receiver_(static_cast<R&&>(r)), ring_(std::move(ring)), fd_(fd),
          buf_(buf), nbytes_(nbytes) {}

      void start() noexcept {
        auto* op = new op_state<R>(std::addressof(receiver_), ring_, fd_, buf_,
                                   nbytes_);
        op->submit();
      }
    };

    template<typename R>
      requires receiver_of<R, std::size_t>
    auto connect(R&& r) && {
      return op_handle<std::remove_cvref_t<R>>(static_cast<R&&>(r), ring_, fd_,
                                               buf_, nbytes_);
    }

    template<typename R>
      requires receiver_of<R, std::size_t>
    auto connect(R&& r) const& {
      return op_handle<std::remove_cvref_t<R>>(static_cast<R&&>(r), ring_, fd_,
                                               buf_, nbytes_);
    }

  private:
    friend class io_uring_scheduler;
    read_sender(std::shared_ptr<ring_state> ring, int fd, void* buf,
                unsigned nbytes) noexcept
      : ring_(std::move(ring)), fd_(fd), buf_(buf), nbytes_(nbytes) {}
    std::shared_ptr<ring_state> ring_;
    int fd_;
    void* buf_;
    unsigned nbytes_;
  };

  auto async_read(int fd, void* buf, unsigned nbytes) const noexcept
      -> read_sender {
    return read_sender(state_, fd, buf, nbytes);
  }

  // -- async_write() ---------------------------------------------------------
  // Returns a sender<std::size_t> that submits an IORING_OP_WRITE and completes
  // with the number of bytes written, or set_error on failure.

  class write_sender {
  public:
    using value_type = std::size_t;

    template<typename R>
    class op_state : public op_state_base {
    public:
      R* receiver_;
      std::shared_ptr<ring_state> ring_;
      int fd_;
      const void* buf_;
      unsigned nbytes_;

      op_state(R* r, std::shared_ptr<ring_state> ring, int fd, const void* buf,
               unsigned nbytes)
        : receiver_(r), ring_(std::move(ring)), fd_(fd), buf_(buf),
          nbytes_(nbytes) {}

      void submit() noexcept {
        auto lock = std::lock_guard(ring_->submit_mtx);
        auto* sqe = detail::io_uring_get_sqe(ring_->ring);
        if (!sqe) {
          coro::set_error(std::move(*receiver_),
                          std::make_exception_ptr(std::runtime_error(
                              "io_uring submission queue full")));
          delete this;
          return;
        }
        detail::io_uring_prep_write(sqe, fd_, buf_, nbytes_, 0);
        sqe->user_data = reinterpret_cast<uintptr_t>(this);
        detail::io_uring_sqe_advance(ring_->ring, sqe);
        detail::io_uring_submit(ring_->ring);
      }

      void on_complete(int result) noexcept override {
        if (result < 0) {
          coro::set_error(std::move(*receiver_),
                          std::make_exception_ptr(std::system_error(
                              -result, std::generic_category())));
        } else {
          coro::set_value(std::move(*receiver_),
                          static_cast<std::size_t>(result));
        }
        delete this;
      }
    };

    template<typename R>
    class op_handle {
    public:
      R receiver_;
      std::shared_ptr<ring_state> ring_;
      int fd_;
      const void* buf_;
      unsigned nbytes_;

      op_handle(R&& r, std::shared_ptr<ring_state> ring, int fd,
                const void* buf, unsigned nbytes)
        : receiver_(static_cast<R&&>(r)), ring_(std::move(ring)), fd_(fd),
          buf_(buf), nbytes_(nbytes) {}

      void start() noexcept {
        auto* op = new op_state<R>(std::addressof(receiver_), ring_, fd_, buf_,
                                   nbytes_);
        op->submit();
      }
    };

    template<typename R>
      requires receiver_of<R, std::size_t>
    auto connect(R&& r) && {
      return op_handle<std::remove_cvref_t<R>>(static_cast<R&&>(r), ring_, fd_,
                                               buf_, nbytes_);
    }

    template<typename R>
      requires receiver_of<R, std::size_t>
    auto connect(R&& r) const& {
      return op_handle<std::remove_cvref_t<R>>(static_cast<R&&>(r), ring_, fd_,
                                               buf_, nbytes_);
    }

  private:
    friend class io_uring_scheduler;
    write_sender(std::shared_ptr<ring_state> ring, int fd, const void* buf,
                 unsigned nbytes) noexcept
      : ring_(std::move(ring)), fd_(fd), buf_(buf), nbytes_(nbytes) {}
    std::shared_ptr<ring_state> ring_;
    int fd_;
    const void* buf_;
    unsigned nbytes_;
  };

  auto async_write(int fd, const void* buf, unsigned nbytes) const noexcept
      -> write_sender {
    return write_sender(state_, fd, buf, nbytes);
  }

  // -- async_timeout() -------------------------------------------------------
  // Returns a sender<void> that fires after the given duration.

  class timeout_sender {
  public:
    using value_type = void;

    template<typename R>
    class op_state : public op_state_base {
    public:
      R* receiver_;
      std::shared_ptr<ring_state> ring_;
      struct __kernel_timespec ts_;

      op_state(R* r, std::shared_ptr<ring_state> ring,
               struct __kernel_timespec ts)
        : receiver_(r), ring_(std::move(ring)), ts_(ts) {}

      void submit() noexcept {
        auto lock = std::lock_guard(ring_->submit_mtx);
        auto* sqe = detail::io_uring_get_sqe(ring_->ring);
        if (!sqe) {
          coro::set_error(std::move(*receiver_),
                          std::make_exception_ptr(std::runtime_error(
                              "io_uring submission queue full")));
          delete this;
          return;
        }
        detail::io_uring_prep_timeout(sqe, &ts_);
        sqe->user_data = reinterpret_cast<uintptr_t>(this);
        detail::io_uring_sqe_advance(ring_->ring, sqe);
        detail::io_uring_submit(ring_->ring);
      }

      void on_complete(int result) noexcept override {
        // ETIME (-62) means the timeout fired normally; 0 means a completion
        // event arrived before the timeout (count-based); both are success.
        if (result < 0 && result != -ETIME) {
          coro::set_error(std::move(*receiver_),
                          std::make_exception_ptr(std::system_error(
                              -result, std::generic_category())));
        } else {
          coro::set_value(std::move(*receiver_));
        }
        delete this;
      }
    };

    template<typename R>
    class op_handle {
    public:
      R receiver_;
      std::shared_ptr<ring_state> ring_;
      struct __kernel_timespec ts_;

      op_handle(R&& r, std::shared_ptr<ring_state> ring,
                struct __kernel_timespec ts)
        : receiver_(static_cast<R&&>(r)), ring_(std::move(ring)), ts_(ts) {}

      void start() noexcept {
        auto* op =
            new op_state<R>(std::addressof(receiver_), ring_, ts_);
        op->submit();
      }
    };

    template<typename R>
      requires receiver_of<R>
    auto connect(R&& r) && {
      return op_handle<std::remove_cvref_t<R>>(static_cast<R&&>(r), ring_,
                                               ts_);
    }

    template<typename R>
      requires receiver_of<R>
    auto connect(R&& r) const& {
      return op_handle<std::remove_cvref_t<R>>(static_cast<R&&>(r), ring_,
                                               ts_);
    }

  private:
    friend class io_uring_scheduler;
    timeout_sender(std::shared_ptr<ring_state> ring,
                   struct __kernel_timespec ts) noexcept
      : ring_(std::move(ring)), ts_(ts) {}
    std::shared_ptr<ring_state> ring_;
    struct __kernel_timespec ts_;
  };

  auto async_timeout(std::chrono::nanoseconds dur) const noexcept
      -> timeout_sender {
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(dur);
    auto nsecs = dur - secs;
    struct __kernel_timespec ts {
      .tv_sec = static_cast<long long>(secs.count()),
      .tv_nsec = static_cast<long long>(nsecs.count())
    };
    return timeout_sender(state_, ts);
  }

private:
  std::shared_ptr<ring_state> state_;
};

// -- event loop --------------------------------------------------------------

inline void io_uring_scheduler::ring_state::run_event_loop() {
  while (!stopping.load(std::memory_order_acquire)) {
    auto* cqe = detail::io_uring_wait_cqe(ring);
    if (!cqe) {
      continue;
    }

    auto* op = reinterpret_cast<op_state_base*>(cqe->user_data);
    if (op) {
      op->on_complete(cqe->res);
    }

    detail::io_uring_cqe_seen(ring, cqe);
  }
}

} // namespace coro
