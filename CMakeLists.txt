# Copyright 2026 Anuj Attri
# Licensed under the Apache License, Version 2.0 (the "License");
# You may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#     http://www.apache.org/licenses/LICENSE-2.0
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20)
project(lns_gorilla_timeseries CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Compiler flags ────────────────────────────────────────────────────────────
if(MSVC)
    set(OPT_FLAGS    /O2 /arch:AVX2 /DNDEBUG /GL /fp:precise)
    set(ASAN_FLAGS   /Od /Zi)   # MSVC ASAN requires VS 2019+ toolchain; skip for portability
    set(ASAN_LDFLAGS "")
elseif(WIN32)
    # MinGW / UCRT on Windows: no ASan/UBSan libraries shipped by default
    set(OPT_FLAGS    -O3 -march=native -DNDEBUG -ffast-math)
    set(ASAN_FLAGS   -O0 -g)
    set(ASAN_LDFLAGS "")
else()
    set(OPT_FLAGS    -O3 -march=native -DNDEBUG -ffast-math)
    set(ASAN_FLAGS   -O0 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
    set(ASAN_LDFLAGS -fsanitize=address,undefined)
endif()

# ── FetchContent dependencies ─────────────────────────────────────────────────
include(FetchContent)

# Google Benchmark
FetchContent_Declare(benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.9.1
    GIT_SHALLOW    TRUE
)
set(BENCHMARK_ENABLE_TESTING    OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL    OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_WERROR     OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(benchmark)

# Catch2 v3
FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.6.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)

# ALP — only available with Clang (ALP's own requirement)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    FetchContent_Declare(ALP
        GIT_REPOSITORY https://github.com/cwida/ALP.git
        GIT_TAG        main
        GIT_SHALLOW    TRUE
    )
    set(ALP_BUILD_TESTING  OFF CACHE BOOL "" FORCE)
    set(ALP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(ALP)
    set(ALP_AVAILABLE TRUE)
    set(ALP_INCLUDE_DIR ${alp_SOURCE_DIR}/include)
    message(STATUS "ALP: enabled (Clang detected)")
else()
    set(ALP_AVAILABLE FALSE)
    set(ALP_INCLUDE_DIR "")
    message(STATUS "ALP: disabled (requires Clang, got ${CMAKE_CXX_COMPILER_ID}) — alp_codec falls back to IEEE baseline")
endif()

# ── Shared codec library ──────────────────────────────────────────────────────
add_library(lns_codecs STATIC
    src/lns_codec.cpp
    src/gorilla_codec.cpp
    src/composite.cpp
)
target_include_directories(lns_codecs PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
if(ALP_AVAILABLE)
    target_include_directories(lns_codecs PUBLIC ${ALP_INCLUDE_DIR})
    target_compile_definitions(lns_codecs PUBLIC LNS_HAS_ALP=1)
else()
    target_compile_definitions(lns_codecs PUBLIC LNS_HAS_ALP=0)
endif()
target_compile_options(lns_codecs PRIVATE ${OPT_FLAGS})

# ── Unit tests ────────────────────────────────────────────────────────────────
add_executable(tests
    tests/test_lns_roundtrip.cpp
    tests/test_gorilla.cpp
    tests/test_composite.cpp
    tests/test_probe.cpp
)
target_include_directories(tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(tests PRIVATE lns_codecs Catch2::Catch2WithMain)
target_compile_options(tests PRIVATE ${ASAN_FLAGS})
if(ASAN_LDFLAGS)
    target_link_options(tests PRIVATE ${ASAN_LDFLAGS})
endif()

# ── Benchmarks ────────────────────────────────────────────────────────────────
add_executable(bench_codecs bench/bench_codecs.cpp)
target_include_directories(bench_codecs PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(bench_codecs PRIVATE lns_codecs benchmark::benchmark benchmark::benchmark_main)
target_compile_options(bench_codecs PRIVATE ${OPT_FLAGS})

add_executable(ratio_sweep bench/ratio_sweep.cpp)
target_include_directories(ratio_sweep PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(ratio_sweep PRIVATE lns_codecs)
target_compile_options(ratio_sweep PRIVATE ${OPT_FLAGS})

add_executable(stat_sig bench/stat_sig.cpp)
target_include_directories(stat_sig PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(stat_sig PRIVATE lns_codecs)
target_compile_options(stat_sig PRIVATE ${OPT_FLAGS})

add_executable(bench_probe bench/bench_probe.cpp)
target_include_directories(bench_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(bench_probe PRIVATE benchmark::benchmark benchmark::benchmark_main)
target_compile_options(bench_probe PRIVATE ${OPT_FLAGS})

# ── Tools ─────────────────────────────────────────────────────────────────────
add_executable(gen_synthetic tools/gen_synthetic.cpp)
target_include_directories(gen_synthetic PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(gen_synthetic PRIVATE lns_codecs)
target_compile_options(gen_synthetic PRIVATE ${OPT_FLAGS})

add_executable(csv_to_bin tools/csv_to_bin.cpp)
target_include_directories(csv_to_bin PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(csv_to_bin PRIVATE ${OPT_FLAGS})

# ── CTest integration ─────────────────────────────────────────────────────────
include(CTest)
include(Catch)
catch_discover_tests(tests)
