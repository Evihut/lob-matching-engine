cmake_minimum_required(VERSION 3.20)
project(lob_engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(LOB_BUILD_TESTS "Build unit tests" ON)
option(LOB_BUILD_BENCH "Build benchmarks" ON)
option(LOB_SANITIZE "Build with AddressSanitizer + UBSan" OFF)
option(LOB_NATIVE "Tune for the build machine (-march=native / -mcpu=native)" OFF)

add_library(lob INTERFACE)
add_library(lob::lob ALIAS lob)
target_include_directories(lob INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(lob INTERFACE cxx_std_20)

add_library(lob_warnings INTERFACE)
target_compile_options(lob_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
if(LOB_SANITIZE)
  target_compile_options(lob_warnings INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(lob_warnings INTERFACE -fsanitize=address,undefined)
endif()
if(LOB_NATIVE)
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag(-march=native HAS_MARCH_NATIVE)
  if(HAS_MARCH_NATIVE)
    target_compile_options(lob_warnings INTERFACE -march=native)
  else()
    target_compile_options(lob_warnings INTERFACE -mcpu=native)
  endif()
endif()

add_executable(lob_replay tools/replay.cpp)
target_link_libraries(lob_replay PRIVATE lob lob_warnings)

add_executable(lob_gen tools/gen_flow.cpp)
target_link_libraries(lob_gen PRIVATE lob lob_warnings)

add_executable(lob_example examples/basic.cpp)
target_link_libraries(lob_example PRIVATE lob lob_warnings)

include(FetchContent)

if(LOB_BUILD_TESTS)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE SYSTEM)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_executable(lob_tests
    tests/test_order_book.cpp
    tests/test_differential.cpp
    tests/test_id_map_and_parsing.cpp)
  target_link_libraries(lob_tests PRIVATE lob lob_warnings GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(lob_tests)
  add_test(NAME example COMMAND lob_example)
endif()

if(LOB_BUILD_BENCH)
  FetchContent_Declare(benchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.1.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE SYSTEM)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(benchmark)

  add_executable(lob_bench bench/bench_order_book.cpp)
  target_link_libraries(lob_bench PRIVATE lob lob_warnings benchmark::benchmark)
endif()
