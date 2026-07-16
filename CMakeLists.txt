cmake_minimum_required(VERSION 3.20)
project(itch_orderbook CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Match Homebrew bottles' deployment target so the linker stays quiet on macOS.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0)
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

option(ITCH_BUILD_TESTS "Build unit tests (needs Catch2)" ON)
option(ITCH_BUILD_BENCH "Build benchmarks (needs Google Benchmark)" ON)
option(ITCH_ASAN "Build with Address/UB sanitizers" OFF)
option(ITCH_WERROR "Treat warnings as errors" OFF)

add_compile_options(-Wall -Wextra -Wpedantic)
if(ITCH_WERROR)
    add_compile_options(-Werror)
endif()

# Header-only core library.
add_library(itch INTERFACE)
target_include_directories(itch INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(itch INTERFACE cxx_std_20)

if(ITCH_ASAN)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

# CLI replayer.
add_executable(itch_replay src/main.cpp)
target_link_libraries(itch_replay PRIVATE itch)

# Synthetic test-data generator (no external data needed).
add_executable(itch_gen tools/gen_synthetic.cpp)
target_link_libraries(itch_gen PRIVATE itch)

# Live MoldUDP64 multicast client (same decode+book path as replay, network
# source). Builds everywhere POSIX sockets exist; needs a real feed to do work.
add_executable(itch_live src/live.cpp)
target_link_libraries(itch_live PRIVATE itch)

# Allocator-isolation benchmark behind the README's cross-platform study. Uses a
# built-in timing harness (no Google Benchmark), so it always builds.
add_executable(itch_bench_levels bench/bench_levels.cpp)
target_link_libraries(itch_bench_levels PRIVATE itch)

# --- Optional: unit tests -----------------------------------------------------
if(ITCH_BUILD_TESTS)
    find_package(Catch2 3 QUIET)
    if(Catch2_FOUND)
        enable_testing()
        add_executable(itch_tests tests/test_itch.cpp)
        target_link_libraries(itch_tests PRIVATE itch Catch2::Catch2WithMain)
        add_test(NAME itch_tests COMMAND itch_tests)
    else()
        message(STATUS "Catch2 not found -- skipping tests (brew install catch2)")
    endif()
endif()

# --- Optional: benchmarks -----------------------------------------------------
if(ITCH_BUILD_BENCH)
    find_package(benchmark QUIET)
    if(benchmark_FOUND)
        add_executable(itch_bench bench/bench_book.cpp)
        target_link_libraries(itch_bench PRIVATE itch benchmark::benchmark)
    else()
        message(STATUS "Google Benchmark not found -- skipping bench (brew install google-benchmark)")
    endif()
endif()
