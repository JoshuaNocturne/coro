#pragma once

/// @file config.hpp
/// Compiler and platform detection for the coro library.

#if !defined(__cpp_coroutines) && !defined(__cpp_lib_coroutine)
#if __has_include(<coroutine>)
#include <coroutine>
#elif __has_include(<experimental/coroutine>)
#include <experimental/coroutine>
namespace std {
using namespace experimental;
} // namespace std
#else
#error "Coroutine support not found"
#endif
#endif

#if !defined(__cpp_concepts)
#error "C++20 concepts support is required"
#endif

#define CORO_VERSION_MAJOR 0
#define CORO_VERSION_MINOR 1
#define CORO_VERSION_PATCH 0
