#pragma once

/// @file coro.hpp
/// Aggregate header for the coro library.
///
/// Include this single header to get everything:

#include <coro/awaitable.hpp>
#include <coro/config.hpp>
#include <coro/inline_scheduler.hpp>
#include <coro/io_uring_scheduler.hpp>
#include <coro/receiver.hpp>
#include <coro/scheduler.hpp>
#include <coro/sender.hpp>
#include <coro/sync_wait.hpp>
#include <coro/task.hpp>
#include <coro/then.hpp>
#include <coro/let_value.hpp>
#include <coro/thread_pool_scheduler.hpp>
#include <coro/when_all.hpp>
