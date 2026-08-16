// Shared benchmark core for comparing exception propagation in nested
// generators:
//   - bench_old.cpp includes the stock generator.hpp (rethrow through every
//     frame),
//   - bench_new.cpp includes generator_improved.hpp (cancellation chain).
//
// Scenario: root -> mid -> leaf. The leaf throws after yielding a few
// elements. The exception must propagate to the caller of the root iterator.
// The benchmark measures the total time of many rounds, plus a no-exception
// baseline to show that the normal path is unaffected.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ranges>

// A lightweight exception type (no string allocation).
struct boom {};

static int sink = 0;

std::generator<int> leaf(int n) {
  for (int i = 0; i < n; ++i)
    co_yield i;
  throw boom{};
}

std::generator<int> mid(int n) {
  co_yield 0;
  co_yield std::ranges::elements_of(leaf(n));
  co_yield 1;
}

std::generator<int> root(int n) {
  co_yield 0;
  co_yield std::ranges::elements_of(mid(n));
  co_yield 1;
}

// Non-throwing variants for the normal-path baseline. The throwing leaf above
// unconditionally throws, so the baseline must use a separate pipeline that
// never throws.
std::generator<int> leaf_ok(int n) {
  for (int i = 0; i < n; ++i)
    co_yield i;
}

std::generator<int> mid_ok(int n) {
  co_yield 0;
  co_yield std::ranges::elements_of(leaf_ok(n));
  co_yield 1;
}

std::generator<int> root_ok(int n) {
  co_yield 0;
  co_yield std::ranges::elements_of(mid_ok(n));
  co_yield 1;
}

// Run `rounds` iterations of the failing pipeline; every round must observe
// exactly one boom.
static long long run_failing(int rounds) {
  int caught = 0;
  auto t0 = std::chrono::steady_clock::now();
  for (int r = 0; r < rounds; ++r) {
    try {
      auto g = root(3);
      for (int v : g) {
        sink += v;
      }
    } catch (const boom &) {
      ++caught;
    }
  }
  auto t1 = std::chrono::steady_clock::now();
  if (caught != rounds) {
    std::printf("FAIL: expected %d catches, got %d\n", rounds, caught);
    return -1;
  }
  return std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
}

// Baseline: the same shape without throwing, to show the normal path is not
// slowed down by the cancellation machinery.
static long long run_normal(int rounds) {
  auto t0 = std::chrono::steady_clock::now();
  for (int r = 0; r < rounds; ++r) {
    auto g = root_ok(3);
    for (int v : g) {
      sink += v;
    }
  }
  auto t1 = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
}

int main(int argc, char **argv) {
  int rounds = argc > 1 ? std::atoi(argv[1]) : 100000;
  int depth = 3; // root -> mid -> leaf

  std::printf("rounds=%d depth=%d\n", rounds, depth);
  std::printf("impl: %s\n", GENERATOR_IMPL_NAME);

  long long normal_ns = run_normal(rounds);
  long long failing_ns = run_failing(rounds);
  if (failing_ns < 0)
    return 1;

  std::printf("normal : %10lld ns total, %8.1f ns/round\n", normal_ns,
              (double)normal_ns / rounds);
  std::printf("failing: %10lld ns total, %8.1f ns/round\n", failing_ns,
              (double)failing_ns / rounds);
  std::printf("overhead per failing round: %8.1f ns\n",
              (double)(failing_ns - normal_ns) / rounds);
  std::printf("sink=%d\n", sink);
  return 0;
}
