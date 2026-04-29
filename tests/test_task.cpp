#include <stdexcept>
#include <string>

#include <coro/coro.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace coro;

// ---------------------------------------------------------------------------
//  task<T> — basic construction and co_return
// ---------------------------------------------------------------------------

task<int> simple_int_task() {
  co_return 42;
}

task<void> simple_void_task() {
  co_return;
}

task<std::string> string_task() {
  co_return "hello";
}

TEST_CASE("task<int> co_returns value via sync_wait", "[task]") {
  auto result = sync_wait(simple_int_task());
  REQUIRE(result == 42);
}

TEST_CASE("task<void> completes via sync_wait", "[task]") {
  // Should not throw
  sync_wait(simple_void_task());
}

TEST_CASE("task<string> co_returns value via sync_wait", "[task]") {
  auto result = sync_wait(string_task());
  REQUIRE(result == "hello");
}

// ---------------------------------------------------------------------------
//  task<T> — exception propagation
// ---------------------------------------------------------------------------

task<int> throwing_task() {
  throw std::runtime_error("boom");
  co_return 0;
}

TEST_CASE("task propagates exceptions through sync_wait", "[task]") {
  REQUIRE_THROWS_AS(sync_wait(throwing_task()), std::runtime_error);
}

// ---------------------------------------------------------------------------
//  task<T> — co_await chaining
// ---------------------------------------------------------------------------

task<int> add_one(int x) {
  co_return x + 1;
}

task<int> chained_task() {
  int a = co_await simple_int_task();
  int b = co_await add_one(a);
  co_return b;
}

TEST_CASE("task chaining with co_await", "[task]") {
  auto result = sync_wait(chained_task());
  REQUIRE(result == 43);
}

// ---------------------------------------------------------------------------
//  task<void> — co_await void task
// ---------------------------------------------------------------------------

task<int> await_void_then_return() {
  co_await simple_void_task();
  co_return 99;
}

TEST_CASE("co_await void task then return", "[task]") {
  auto result = sync_wait(await_void_then_return());
  REQUIRE(result == 99);
}

// ---------------------------------------------------------------------------
//  then combinator
// ---------------------------------------------------------------------------

TEST_CASE("then transforms task result", "[then]") {
  auto t = simple_int_task() | then([](int x) { return x * 2; });
  auto result = sync_wait(std::move(t));
  REQUIRE(result == 84);
}

TEST_CASE("then with different return type", "[then]") {
  auto t = simple_int_task() | then([](int x) { return std::to_string(x); });
  auto result = sync_wait(std::move(t));
  REQUIRE(result == "42");
}

// ---------------------------------------------------------------------------
//  when_all combinator
// ---------------------------------------------------------------------------

task<int> int_10() {
  co_return 10;
}
task<int> int_20() {
  co_return 20;
}
task<std::string> str_task() {
  co_return "abc";
}

TEST_CASE("when_all with two int tasks", "[when_all]") {
  auto result = sync_wait(when_all(int_10(), int_20()));
  REQUIRE(result == std::make_tuple(10, 20));
}

TEST_CASE("when_all with mixed types", "[when_all]") {
  auto result = sync_wait(when_all(int_10(), str_task()));
  REQUIRE(std::get<0>(result) == 10);
  REQUIRE(std::get<1>(result) == "abc");
}

// ---------------------------------------------------------------------------
//  Sender/Receiver: connect + start on task directly
// ---------------------------------------------------------------------------

TEST_CASE("task as sender: connect + start", "[sender]") {
  // Manually use the sender/receiver protocol instead of sync_wait
  int captured = 0;
  std::exception_ptr error;
  bool done = false;

  struct test_receiver {
    int& out;
    std::exception_ptr& err;
    bool& done_flag;

    void set_value(int v) {
      out = v;
      done_flag = true;
    }
    void set_error(std::exception_ptr e) noexcept {
      err = std::move(e);
      done_flag = true;
    }
    void set_stopped() noexcept {
      done_flag = true;
    }
  };

  auto t = simple_int_task();
  auto op = std::move(t).connect(test_receiver{captured, error, done});
  start(op);

  REQUIRE(done);
  REQUIRE(captured == 42);
  REQUIRE(!error);
}

// ---------------------------------------------------------------------------
//  Complex chaining
// ---------------------------------------------------------------------------

task<int> fibonacci(int n) {
  if (n <= 1)
    co_return n;
  auto a = co_await fibonacci(n - 1);
  auto b = co_await fibonacci(n - 2);
  co_return a + b;
}

TEST_CASE("recursive fibonacci with task", "[task]") {
  auto result = sync_wait(fibonacci(10));
  REQUIRE(result == 55);
}
