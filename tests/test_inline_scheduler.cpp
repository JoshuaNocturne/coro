#include <coro/coro.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace coro;

// ---------------------------------------------------------------------------
//  inline_scheduler
// ---------------------------------------------------------------------------

TEST_CASE("inline_scheduler schedule completes", "[scheduler][inline]") {
  inline_scheduler sched;
  sync_wait(sched.schedule());
  // void result — just check no throw
}
