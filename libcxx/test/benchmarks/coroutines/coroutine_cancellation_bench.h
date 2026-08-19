//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Shared benchmark body for nested-generator exception propagation
// (PxxxxR0, collaborative cancellation for coroutines).
//
// Two entry files instantiate this with different generator implementations:
//   - coroutine_cancellation.bench.cpp        -> generator_improved.hpp
//     (cancellation chain: the exception is stored once at the source and
//     handled once at the root; intermediate frames only set the
//     cancellation flag and transfer to the parent via symmetric transfer)
//   - coroutine_cancellation_stock.bench.cpp  -> generator.hpp
//     (stock behavior: one throw/catch pair per nested frame)
//
// Both implementations are compiled against the proposal-modified libc++
// (coroutine_handle with request_cancel / cancel_requested /
// unhandled_cancellation support); the stock variant exercises the
// non-cancellation-aware path of the same library.
//
// The chain depth is a compile-time template parameter. The innermost
// generator (Depth == 0) yields `n` elements and then throws; every
// intermediate frame yields one element around the nested generator and one
// after it. Each iteration of a benchmark must observe exactly one `boom`.
// The non-throwing variant is the baseline for the normal path.

#include <benchmark/benchmark.h>
#include <ranges>

#include "test_macros.h"

// A lightweight exception type (no string allocation).
struct boom {};

static int sink = 0;

template <int Depth> std::generator<int> chain(int n) {
  if constexpr (Depth == 0) {
    for (int i = 0; i < n; ++i)
      co_yield i;
    throw boom{};
  } else {
    co_yield 0;
    co_yield std::ranges::elements_of(chain<Depth - 1>(n));
    co_yield 1;
  }
}

// Non-throwing variant for the normal-path baseline.
template <int Depth> std::generator<int> chain_ok(int n) {
  if constexpr (Depth == 0) {
    for (int i = 0; i < n; ++i)
      co_yield i;
  } else {
    co_yield 0;
    co_yield std::ranges::elements_of(chain_ok<Depth - 1>(n));
    co_yield 1;
  }
}

// Failing path: every iteration must propagate exactly one boom from the
// innermost generator to the caller of the root iterator.
template <int Depth>
static TEST_ALIGN_BENCHMARK void BM_nested_generator_failing(benchmark::State& state) {
  int caught = 0;
  for (auto _ : state) {
    try {
      auto g = chain<Depth>(3);
      for (int v : g)
        sink += v;
    } catch (const boom&) {
      ++caught;
    }
  }
  benchmark::DoNotOptimize(sink);
  if (caught != static_cast<int>(state.iterations()))
    state.SkipWithError("expected exactly one boom per iteration");
}

// Normal path: the same shape without throwing.
template <int Depth>
static TEST_ALIGN_BENCHMARK void BM_nested_generator_normal(benchmark::State& state) {
  for (auto _ : state) {
    auto g = chain_ok<Depth>(3);
    for (int v : g)
      sink += v;
  }
  benchmark::DoNotOptimize(sink);
}

// Baselines without nesting: a single (non-nested) generator, with and
// without throwing. Depth 0 is the leaf of `chain` / `chain_ok`.
static TEST_ALIGN_BENCHMARK void BM_baseline_generator_failing(benchmark::State& state) {
  int caught = 0;
  for (auto _ : state) {
    try {
      auto g = chain<0>(3);
      for (int v : g)
        sink += v;
    } catch (const boom&) {
      ++caught;
    }
  }
  benchmark::DoNotOptimize(sink);
  if (caught != static_cast<int>(state.iterations()))
    state.SkipWithError("expected exactly one boom per iteration");
}

static TEST_ALIGN_BENCHMARK void BM_baseline_generator_normal(benchmark::State& state) {
  for (auto _ : state) {
    auto g = chain_ok<0>(3);
    for (int v : g)
      sink += v;
  }
  benchmark::DoNotOptimize(sink);
}

#define BENCH_NAME(kind, depth) "nested_generator/" IMPL_TAG "/" kind "/depth" depth

BENCHMARK_TEMPLATE(BM_nested_generator_failing, 3)->Name(BENCH_NAME("failing", "3"));
BENCHMARK_TEMPLATE(BM_nested_generator_failing, 5)->Name(BENCH_NAME("failing", "5"));
BENCHMARK_TEMPLATE(BM_nested_generator_normal, 3)->Name(BENCH_NAME("normal", "3"));
BENCHMARK_TEMPLATE(BM_nested_generator_normal, 5)->Name(BENCH_NAME("normal", "5"));

// Implementation-independent baselines (identical in both entry files):
// depth 0 = single generator, no nesting.
BENCHMARK(BM_baseline_generator_failing)->Name("baseline/single_generator/failing");
BENCHMARK(BM_baseline_generator_normal)->Name("baseline/single_generator/normal");

BENCHMARK_MAIN();
