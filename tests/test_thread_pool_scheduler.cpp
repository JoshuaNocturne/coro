#include <atomic>
#include <string>

#include <coro/coro.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace coro;

// ---------------------------------------------------------------------------
//  thread_pool_scheduler — basic
// ---------------------------------------------------------------------------

TEST_CASE("thread_pool_scheduler schedule completes", "[scheduler][thread_pool]") {
  thread_pool_scheduler pool(2);
  sync_wait(pool.schedule());
}

TEST_CASE("thread_pool_scheduler schedule with then", "[scheduler][thread_pool]") {
  thread_pool_scheduler pool(2);
  auto t = pool.schedule() | then([] { return 42; });
  auto result = sync_wait(std::move(t));
  REQUIRE(result == 42);
}

TEST_CASE("thread_pool_scheduler runs on worker thread", "[scheduler][thread_pool]") {
  thread_pool_scheduler pool(2);
  auto t = pool.schedule() | then([] {
             return std::hash<std::thread::id>{}(std::this_thread::get_id());
           });
  auto worker_id_hash = sync_wait(std::move(t));
  auto main_id_hash = std::hash<std::thread::id>{}(std::this_thread::get_id());
  REQUIRE(worker_id_hash != main_id_hash);
}

TEST_CASE("thread_pool_scheduler multiple tasks", "[scheduler][thread_pool]") {
  thread_pool_scheduler pool(2);
  auto t1 = pool.schedule() | then([] { return 1; });
  auto t2 = pool.schedule() | then([] { return 2; });
  auto t3 = pool.schedule() | then([] { return 3; });
  auto r1 = sync_wait(std::move(t1));
  auto r2 = sync_wait(std::move(t2));
  auto r3 = sync_wait(std::move(t3));
  REQUIRE(r1 == 1);
  REQUIRE(r2 == 2);
  REQUIRE(r3 == 3);
}

TEST_CASE("thread_pool_scheduler with then chaining", "[scheduler][thread_pool]") {
  thread_pool_scheduler pool(2);
  auto t = pool.schedule() | then([] { return 10; }) |
           then([](int x) { return x + 20; });
  auto result = sync_wait(std::move(t));
  REQUIRE(result == 30);
}

TEST_CASE("thread_pool_scheduler with when_all", "[scheduler][thread_pool]") {
  thread_pool_scheduler pool(2);

  // then + thread_pool works
  auto t1 = pool.schedule() | then([] { return 10; });
  auto r1 = sync_wait(std::move(t1));
  REQUIRE(r1 == 10);

  // when_all + inline + then works
  inline_scheduler is;
  auto t2 = when_all(is.schedule() | then([] { return 10; }),
                     is.schedule() | then([] { return 20; }));
  auto r2 = sync_wait(std::move(t2));
  REQUIRE(r2 == std::make_tuple(10, 20));

  // when_all + thread_pool + then
  auto t3 = when_all(pool.schedule() | then([] { return 10; }),
                     pool.schedule() | then([] { return 20; }));
  auto r3 = sync_wait(std::move(t3));
  REQUIRE(std::get<0>(r3) == 10);
  REQUIRE(std::get<1>(r3) == 20);
}

// ---------------------------------------------------------------------------
//  thread_pool_scheduler + coroutines
// ---------------------------------------------------------------------------

task<int> pool_int_task(int x) {
  co_return x;
}

task<void> pool_void_task() {
  co_return;
}

task<std::string> pool_string_task() {
  co_return "from pool";
}

TEST_CASE("thread_pool_scheduler executes task via sync_wait",
          "[scheduler][thread_pool][coro]") {
  thread_pool_scheduler pool(2);
  auto t = pool.schedule() | then([] { return pool_int_task(42); });
  auto inner = sync_wait(std::move(t));
  auto result = sync_wait(std::move(inner));
  REQUIRE(result == 42);
}

TEST_CASE("thread_pool_scheduler executes void task",
          "[scheduler][thread_pool][coro]") {
  thread_pool_scheduler pool(2);
  bool completed = false;
  auto t = pool.schedule() | then([&] {
             sync_wait(pool_void_task());
             completed = true;
           });
  sync_wait(std::move(t));
  REQUIRE(completed);
}

TEST_CASE("thread_pool_scheduler with task in then chain",
          "[scheduler][thread_pool][coro]") {
  thread_pool_scheduler pool(2);
  // then returns task<int>, so the sender's value_type becomes task<int>
  auto t = pool.schedule() | then([] { return pool_int_task(42); });
  auto inner_task = sync_wait(std::move(t));
  auto result = sync_wait(std::move(inner_task));
  REQUIRE(result == 42);
}

TEST_CASE("thread_pool_scheduler concurrent task execution",
          "[scheduler][thread_pool][coro]") {
  thread_pool_scheduler pool(4);

  std::atomic<int> counter{0};
  auto make_task = [&counter](int id) -> task<int> {
    counter.fetch_add(1, std::memory_order_relaxed);
    co_return id * 10;
  };

  auto t1 = pool.schedule() | then([&] { return make_task(1); });
  auto t2 = pool.schedule() | then([&] { return make_task(2); });
  auto t3 = pool.schedule() | then([&] { return make_task(3); });

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

TEST_CASE("thread_pool_scheduler with when_all and tasks",
          "[scheduler][thread_pool][coro]") {
  thread_pool_scheduler pool(2);

  auto t = when_all(pool.schedule() | then([] { return pool_int_task(1); }),
                    pool.schedule() | then([] { return pool_int_task(2); }));
  auto [task1, task2] = sync_wait(std::move(t));

  auto r1 = sync_wait(std::move(task1));
  auto r2 = sync_wait(std::move(task2));

  REQUIRE(r1 == 1);
  REQUIRE(r2 == 2);
}

task<int> pool_throwing_task() {
  throw std::runtime_error("pool error");
  co_return 0;
}

TEST_CASE("thread_pool_scheduler task exception propagation",
          "[scheduler][thread_pool][coro]") {
  thread_pool_scheduler pool(2);

  auto t = pool.schedule() | then([] { return pool_throwing_task(); });
  auto inner = sync_wait(std::move(t));
  REQUIRE_THROWS_AS(sync_wait(std::move(inner)), std::runtime_error);
}

TEST_CASE("thread_pool_scheduler runs coroutines on worker threads",
          "[scheduler][thread_pool][coro]") {
  thread_pool_scheduler pool(2);

  std::thread::id coro_thread_id;
  auto t = pool.schedule() | then([&] {
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
