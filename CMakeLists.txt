cmake_minimum_required(VERSION 3.20)
project(coro LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
#  Build type configuration
# ---------------------------------------------------------------------------
# Set default build type to Debug if not specified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "Setting build type to 'Debug' as none was specified.")
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

# Compiler flags for Debug and Release builds
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG" CACHE STRING "Debug flags" FORCE)
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG" CACHE STRING "Release flags" FORCE)
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG" CACHE STRING "RelWithDebInfo flags" FORCE)
    set(CMAKE_CXX_FLAGS_MINSIZEREL "-Os -DNDEBUG" CACHE STRING "MinSizeRel flags" FORCE)
elseif(MSVC)
    set(CMAKE_CXX_FLAGS_DEBUG "/Od /Zi /D_DEBUG" CACHE STRING "Debug flags" FORCE)
    set(CMAKE_CXX_FLAGS_RELEASE "/O2 /DNDEBUG" CACHE STRING "Release flags" FORCE)
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "/O2 /Zi /DNDEBUG" CACHE STRING "RelWithDebInfo flags" FORCE)
    set(CMAKE_CXX_FLAGS_MINSIZEREL "/O1 /DNDEBUG" CACHE STRING "MinSizeRel flags" FORCE)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# ---------------------------------------------------------------------------
#  Compile commands database (for clangd, cquery, etc.)
# ---------------------------------------------------------------------------
option(CORO_EXPORT_COMPILE_COMMANDS "Generate compile_commands.json" ON)
if(CORO_EXPORT_COMPILE_COMMANDS)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

    # Only Makefile and Ninja generators support this natively.
    # Warn the user if a different generator is in use.
    if(NOT CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
        message(WARNING "CMAKE_EXPORT_COMPILE_COMMANDS is only supported by Makefile and Ninja generators.")
    endif()

    # Symlink compile_commands.json to the source root so clangd can find it
    # regardless of the build directory location.
    if(CMAKE_EXPORT_COMPILE_COMMANDS)
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                ${CMAKE_BINARY_DIR}/compile_commands.json
                ${CMAKE_SOURCE_DIR}/compile_commands.json
            ERROR_QUIET
        )
    endif()
endif()

# Header-only library
add_library(coro INTERFACE)
target_include_directories(coro INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Compiler warnings
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(coro INTERFACE -Wall -Wextra -Wpedantic)
endif()

# Tests
option(CORO_BUILD_TESTS "Build tests" ON)
if(CORO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
