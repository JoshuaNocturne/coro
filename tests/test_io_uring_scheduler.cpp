#include <atomic>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

#include <coro/coro.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace coro;

// ---------------------------------------------------------------------------
//  io_uring_scheduler
// ---------------------------------------------------------------------------

TEST_CASE("io_uring_scheduler schedule completes", "[scheduler][io_uring]") {
  io_uring_scheduler uring(256);
  sync_wait(uring.schedule());
}

TEST_CASE("io_uring_scheduler schedule with then", "[scheduler][io_uring]") {
  io_uring_scheduler uring(256);
  auto t = uring.schedule() | then([] { return 42; });
  auto result = sync_wait(std::move(t));
  REQUIRE(result == 42);
}

TEST_CASE("io_uring_scheduler runs on event loop thread",
          "[scheduler][io_uring]") {
  io_uring_scheduler uring(256);
  std::thread::id uring_thread_id;
  auto t = uring.schedule() |
           then([&] { uring_thread_id = std::this_thread::get_id(); });
  sync_wait(std::move(t));

  auto main_id = std::this_thread::get_id();
  REQUIRE(uring_thread_id != main_id);
}

TEST_CASE("io_uring_scheduler multiple sequential schedules",
          "[scheduler][io_uring]") {
  io_uring_scheduler uring(256);
  auto r1 = sync_wait(uring.schedule() | then([] { return 1; }));
  auto r2 = sync_wait(uring.schedule() | then([] { return 2; }));
  auto r3 = sync_wait(uring.schedule() | then([] { return 3; }));
  REQUIRE(r1 == 1);
  REQUIRE(r2 == 2);
  REQUIRE(r3 == 3);
}

TEST_CASE("io_uring_scheduler with then chaining", "[scheduler][io_uring]") {
  io_uring_scheduler uring(256);
  auto t = uring.schedule() | then([] { return 10; }) |
           then([](int x) { return x + 20; });
  auto result = sync_wait(std::move(t));
  REQUIRE(result == 30);
}

TEST_CASE("io_uring_scheduler graceful shutdown", "[scheduler][io_uring]") {
  // Just verify that destruction doesn't crash or hang
  {
    io_uring_scheduler uring(256);
    sync_wait(uring.schedule());
  }
  // uring destroyed here — should join event loop thread cleanly
  REQUIRE(true);
}

// ---------------------------------------------------------------------------
//  io_uring_scheduler + coroutines (task)
// ---------------------------------------------------------------------------

task<int> uring_int_task(int x) {
  co_return x;
}

task<void> uring_void_task() {
  co_return;
}

TEST_CASE("io_uring_scheduler executes task via sync_wait",
          "[scheduler][io_uring][coro]") {
  io_uring_scheduler uring(256);
  auto t = uring.schedule() | then([] { return uring_int_task(42); });
  auto inner = sync_wait(std::move(t));
  auto result = sync_wait(std::move(inner));
  REQUIRE(result == 42);
}

TEST_CASE("io_uring_scheduler executes void task",
          "[scheduler][io_uring][coro]") {
  io_uring_scheduler uring(256);
  bool completed = false;
  auto t = uring.schedule() | then([&] {
             sync_wait(uring_void_task());
             completed = true;
           });
  sync_wait(std::move(t));
  REQUIRE(completed);
}

TEST_CASE("io_uring_scheduler with task in then chain",
          "[scheduler][io_uring][coro]") {
  io_uring_scheduler uring(256);
  auto t = uring.schedule() | then([] { return uring_int_task(42); });
  auto inner_task = sync_wait(std::move(t));
  auto result = sync_wait(std::move(inner_task));
  REQUIRE(result == 42);
}

TEST_CASE("io_uring_scheduler concurrent task execution",
          "[scheduler][io_uring][coro]") {
  io_uring_scheduler uring(256);

  std::atomic<int> counter{0};
  auto make_task = [&counter](int id) -> task<int> {
    counter.fetch_add(1, std::memory_order_relaxed);
    co_return id * 10;
  };

  auto t1 = uring.schedule() | then([&] { return make_task(1); });
  auto t2 = uring.schedule() | then([&] { return make_task(2); });
  auto t3 = uring.schedule() | then([&] { return make_task(3); });

  auto r1 = sync_wait(std::move(t1));
  auto r2 = sync_wait(std::move(t2));
  auto r3 = sync_wait(std::move(t3));

  auto v1 = sync_wait(std::move(r1));
  auto v2 = sync_wait(std::move(r2));
  auto v3 = sync_wait(std::move(r3));

  REQUIRE(v1 == 10);
  REQUIRE(v2 == 20);
  REQUIRE(v3 == 30);
  REQUIRE(counter.load() == 3);
}

TEST_CASE("io_uring_scheduler with when_all and tasks",
          "[scheduler][io_uring][coro]") {
  io_uring_scheduler uring(256);

  auto t = when_all(uring.schedule() | then([] { return uring_int_task(1); }),
                    uring.schedule() | then([] { return uring_int_task(2); }));
  auto [task1, task2] = sync_wait(std::move(t));

  auto r1 = sync_wait(std::move(task1));
  auto r2 = sync_wait(std::move(task2));

  REQUIRE(r1 == 1);
  REQUIRE(r2 == 2);
}

task<int> uring_throwing_task() {
  throw std::runtime_error("uring error");
  co_return 0;
}

TEST_CASE("io_uring_scheduler task exception propagation",
          "[scheduler][io_uring][coro]") {
  io_uring_scheduler uring(256);

  auto t = uring.schedule() | then([] { return uring_throwing_task(); });
  auto inner = sync_wait(std::move(t));
  REQUIRE_THROWS_AS(sync_wait(std::move(inner)), std::runtime_error);
}

TEST_CASE("io_uring_scheduler runs coroutines on event loop thread",
          "[scheduler][io_uring][coro]") {
  io_uring_scheduler uring(256);

  std::thread::id coro_thread_id;
  auto t = uring.schedule() | then([&] {
             auto get_id = []() -> task<std::thread::id> {
               co_return std::this_thread::get_id();
             };
             auto task = get_id();
             coro_thread_id = sync_wait(std::move(task));
           });
  sync_wait(std::move(t));

  auto main_id = std::this_thread::get_id();
  REQUIRE(coro_thread_id != main_id);
}

// ---------------------------------------------------------------------------
//  Real I/O: async_write / async_read via pipe
// ---------------------------------------------------------------------------

TEST_CASE("io_uring async_write then async_read via pipe",
          "[scheduler][io_uring][io]") {
  io_uring_scheduler uring(256);

  int fds[2];
  REQUIRE(pipe2(fds, O_CLOEXEC) == 0);
  int rfd = fds[0], wfd = fds[1];

  const std::string msg = "hello io_uring";

  // Write into the pipe
  auto bytes_written = sync_wait(
      uring.async_write(wfd, msg.data(), static_cast<unsigned>(msg.size())));
  REQUIRE(bytes_written == msg.size());

  // Read back from the pipe
  std::string buf(msg.size(), '\0');
  auto bytes_read = sync_wait(
      uring.async_read(rfd, buf.data(), static_cast<unsigned>(buf.size())));
  REQUIRE(bytes_read == msg.size());
  REQUIRE(buf == msg);

  close(rfd);
  close(wfd);
}

TEST_CASE("io_uring async_read returns correct byte count",
          "[scheduler][io_uring][io]") {
  io_uring_scheduler uring(256);

  int fds[2];
  REQUIRE(pipe2(fds, O_CLOEXEC) == 0);
  int rfd = fds[0], wfd = fds[1];

  // Write fewer bytes than the read buffer
  const char* data = "hi";
  auto written =
      sync_wait(uring.async_write(wfd, data, static_cast<unsigned>(2)));
  REQUIRE(written == 2);

  char buf[64] = {};
  auto read_bytes = sync_wait(uring.async_read(rfd, buf, sizeof(buf)));
  REQUIRE(read_bytes == 2);
  REQUIRE(std::string_view(buf, read_bytes) == "hi");

  close(rfd);
  close(wfd);
}

TEST_CASE("io_uring async_write and async_read large data",
          "[scheduler][io_uring][io]") {
  io_uring_scheduler uring(256);

  int fds[2];
  REQUIRE(pipe2(fds, O_CLOEXEC) == 0);
  int rfd = fds[0], wfd = fds[1];

  // Use a 4 KB payload
  std::string payload(4096, 'x');
  for (std::size_t i = 0; i < payload.size(); ++i)
    payload[i] = static_cast<char>('A' + (i % 26));

  // Pipe may have a capacity limit; write and read in a single round trip
  auto written = sync_wait(uring.async_write(
      wfd, payload.data(), static_cast<unsigned>(payload.size())));
  REQUIRE(written == payload.size());

  std::string rbuf(payload.size(), '\0');
  auto bytes_read = sync_wait(
      uring.async_read(rfd, rbuf.data(), static_cast<unsigned>(rbuf.size())));
  REQUIRE(bytes_read == payload.size());
  REQUIRE(rbuf == payload);

  close(rfd);
  close(wfd);
}

TEST_CASE("io_uring async_write and async_read with then combinator",
          "[scheduler][io_uring][io]") {
  io_uring_scheduler uring(256);

  int fds[2];
  REQUIRE(pipe2(fds, O_CLOEXEC) == 0);
  int rfd = fds[0], wfd = fds[1];

  // Chain: write, then map to a string, all via sender pipeline
  const std::string msg = "pipeline";
  auto written = sync_wait(
      uring.async_write(wfd, msg.data(), static_cast<unsigned>(msg.size())) |
      then([](std::size_t n) { return n; }));
  REQUIRE(written == msg.size());

  std::string buf(msg.size(), '\0');
  auto content = sync_wait(
      uring.async_read(rfd, buf.data(), static_cast<unsigned>(buf.size())) |
      then([&buf](std::size_t n) { return buf.substr(0, n); }));
  REQUIRE(content == msg);

  close(rfd);
  close(wfd);
}

TEST_CASE("io_uring async_write and async_read sequential from main thread",
          "[scheduler][io_uring][io]") {
  io_uring_scheduler uring(256);

  int fds[2];
  REQUIRE(pipe2(fds, O_CLOEXEC) == 0);
  int rfd = fds[0], wfd = fds[1];

  const std::string msg = "sequential pipe";

  // Both operations are driven from the main thread via sync_wait;
  // the event loop thread processes the CQEs independently — no deadlock.
  auto written = sync_wait(
      uring.async_write(wfd, msg.data(), static_cast<unsigned>(msg.size())));
  REQUIRE(written == msg.size());

  std::string buf(msg.size(), '\0');
  auto bytes_read = sync_wait(
      uring.async_read(rfd, buf.data(), static_cast<unsigned>(buf.size())));
  REQUIRE(bytes_read == msg.size());
  REQUIRE(buf == msg);

  close(rfd);
  close(wfd);
}

// ---------------------------------------------------------------------------
//  Real I/O: async_timeout
// ---------------------------------------------------------------------------

TEST_CASE("io_uring async_timeout fires and completes",
          "[scheduler][io_uring][timeout]") {
  io_uring_scheduler uring(256);

  using namespace std::chrono;
  auto t0 = steady_clock::now();
  sync_wait(uring.async_timeout(milliseconds(50)));
  auto elapsed = duration_cast<milliseconds>(steady_clock::now() - t0).count();

  // Should have waited at least ~50 ms (allow generous upper bound for CI)
  REQUIRE(elapsed >= 40);
  REQUIRE(elapsed < 2000);
}

TEST_CASE("io_uring async_timeout with then combinator",
          "[scheduler][io_uring][timeout]") {
  io_uring_scheduler uring(256);

  using namespace std::chrono;
  bool fired = false;
  sync_wait(uring.async_timeout(milliseconds(20)) |
            then([&] { fired = true; }));
  REQUIRE(fired);
}

TEST_CASE("io_uring async_timeout ordering: two sequential timeouts",
          "[scheduler][io_uring][timeout]") {
  io_uring_scheduler uring(256);

  using namespace std::chrono;

  auto t0 = steady_clock::now();
  sync_wait(uring.async_timeout(milliseconds(20)));
  sync_wait(uring.async_timeout(milliseconds(20)));
  auto elapsed = duration_cast<milliseconds>(steady_clock::now() - t0).count();

  REQUIRE(elapsed >= 35); // two ~20ms timeouts
  REQUIRE(elapsed < 3000);
}

TEST_CASE("io_uring async_timeout zero duration completes immediately",
          "[scheduler][io_uring][timeout]") {
  io_uring_scheduler uring(256);

  using namespace std::chrono;
  // A zero-duration timeout should still complete (may fire immediately or
  // ETIME)
  sync_wait(uring.async_timeout(nanoseconds(0)));
  REQUIRE(true);
}

// ---------------------------------------------------------------------------
//  Coroutine style: co_await async I/O
// ---------------------------------------------------------------------------

task<void> async_pipe_io(io_uring_scheduler& uring, int wfd, int rfd,
                         std::string& result) {
  const std::string msg = "hello io_uring via co_await";

  // co_await suspends the coroutine, the event loop processes the I/O,
  // and resumes with the result — synchronous code style, async execution.
  auto bytes_written = co_await uring.async_write(
      wfd, msg.data(), static_cast<unsigned>(msg.size()));
  REQUIRE(bytes_written == msg.size());

  std::string buf(64, '\0');
  auto bytes_read = co_await uring.async_read(
      rfd, buf.data(), static_cast<unsigned>(buf.size()));
  result = buf.substr(0, bytes_read);

  close(rfd);
  close(wfd);
}

TEST_CASE("io_uring async I/O via co_await", "[scheduler][io_uring][coro]") {
  io_uring_scheduler uring(256);

  int fds[2];
  REQUIRE(pipe2(fds, O_CLOEXEC) == 0);
  int rfd = fds[0], wfd = fds[1];

  std::string result;
  sync_wait(async_pipe_io(uring, wfd, rfd, result));

  REQUIRE(result == "hello io_uring via co_await");
}

task<void> async_timeout_and_schedule(io_uring_scheduler& uring, bool& fired) {
  using namespace std::chrono;

  // co_await timeout
  co_await uring.async_timeout(milliseconds(20));

  // co_await schedule (NOP, just transfers to event loop thread)
  co_await uring.schedule();

  fired = true;
}

TEST_CASE("io_uring co_await timeout and schedule",
          "[scheduler][io_uring][coro]") {
  io_uring_scheduler uring(256);

  bool fired = false;
  auto t0 = std::chrono::steady_clock::now();
  sync_wait(async_timeout_and_schedule(uring, fired));
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - t0)
                     .count();

  REQUIRE(fired);
  REQUIRE(elapsed >= 15); // at least ~20ms timeout
  REQUIRE(elapsed < 2000);
}

// ---------------------------------------------------------------------------
//  Executor delegation: completions run on user-specified executor thread
// ---------------------------------------------------------------------------

TEST_CASE("io_uring with executor: schedule resumes on pool thread",
          "[scheduler][io_uring][executor]") {
  thread_pool_scheduler pool(2);
  io_uring_scheduler uring(256, pool.executor());

  std::thread::id completion_thread_id;
  auto t = uring.schedule() |
           then([&] { completion_thread_id = std::this_thread::get_id(); });
  sync_wait(std::move(t));

  auto main_id = std::this_thread::get_id();
  REQUIRE(completion_thread_id != main_id);
}

TEST_CASE("io_uring with executor: async_write then async_read via pipe",
          "[scheduler][io_uring][executor][io]") {
  thread_pool_scheduler pool(2);
  io_uring_scheduler uring(256, pool.executor());

  int fds[2];
  REQUIRE(pipe2(fds, O_CLOEXEC) == 0);
  int rfd = fds[0], wfd = fds[1];

  const std::string msg = "hello via executor";
  auto bytes_written = sync_wait(
      uring.async_write(wfd, msg.data(), static_cast<unsigned>(msg.size())));
  REQUIRE(bytes_written == msg.size());

  std::string buf(msg.size(), '\0');
  auto bytes_read = sync_wait(
      uring.async_read(rfd, buf.data(), static_cast<unsigned>(buf.size())));
  REQUIRE(bytes_read == msg.size());
  REQUIRE(buf == msg);

  close(rfd);
  close(wfd);
}

TEST_CASE("io_uring with executor: async_timeout fires",
          "[scheduler][io_uring][executor][timeout]") {
  thread_pool_scheduler pool(2);
  io_uring_scheduler uring(256, pool.executor());

  using namespace std::chrono;
  bool fired = false;
  sync_wait(uring.async_timeout(milliseconds(20)) |
            then([&] { fired = true; }));
  REQUIRE(fired);
}

task<void> executor_pipe_io(io_uring_scheduler& uring, int wfd, int rfd,
                            std::string& result,
                            std::thread::id& resume_thread_id) {
  const std::string msg = "co_await via executor";

  co_await uring.async_write(
      wfd, msg.data(), static_cast<unsigned>(msg.size()));

  std::string buf(64, '\0');
  auto bytes_read = co_await uring.async_read(
      rfd, buf.data(), static_cast<unsigned>(buf.size()));
  result = buf.substr(0, bytes_read);
  resume_thread_id = std::this_thread::get_id();

  close(rfd);
  close(wfd);
}

TEST_CASE("io_uring with executor: co_await resumes on pool thread",
          "[scheduler][io_uring][executor][coro]") {
  thread_pool_scheduler pool(2);
  io_uring_scheduler uring(256, pool.executor());

  int fds[2];
  REQUIRE(pipe2(fds, O_CLOEXEC) == 0);

  std::string result;
  std::thread::id resume_thread_id;
  sync_wait(executor_pipe_io(uring, fds[1], fds[0], result, resume_thread_id));

  REQUIRE(result == "co_await via executor");
  auto main_id = std::this_thread::get_id();
  REQUIRE(resume_thread_id != main_id);
}

TEST_CASE("io_uring with executor: graceful shutdown",
          "[scheduler][io_uring][executor]") {
  thread_pool_scheduler pool(2);
  {
    io_uring_scheduler uring(256, pool.executor());
    sync_wait(uring.schedule());
  }
  // Both uring and pool destroyed — should not crash or hang
}

TEST_CASE("io_uring with inline executor: backward compatible",
          "[scheduler][io_uring][executor]") {
  // Explicit inline executor should behave identically to the default.
  io_uring_scheduler uring(256, [](std::function<void()> f) { f(); });

  std::thread::id completion_thread_id;
  auto t = uring.schedule() |
           then([&] { completion_thread_id = std::this_thread::get_id(); });
  sync_wait(std::move(t));

  auto main_id = std::this_thread::get_id();
  REQUIRE(completion_thread_id != main_id);
}
