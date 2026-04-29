include(FetchContent)

# Fetch Catch2 for testing
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.2
)
FetchContent_MakeAvailable(Catch2)

include(CTest)
include(Catch)

add_executable(test_task             test_task.cpp)
add_executable(test_inline_scheduler test_inline_scheduler.cpp)
add_executable(test_thread_pool      test_thread_pool_scheduler.cpp)
add_executable(test_io_uring         test_io_uring_scheduler.cpp)

foreach(tgt test_task test_inline_scheduler test_thread_pool test_io_uring)
  target_link_libraries(${tgt} PRIVATE coro Catch2::Catch2WithMain)
  target_compile_options(${tgt} PRIVATE -fcoroutines)
  catch_discover_tests(${tgt})
endforeach()
