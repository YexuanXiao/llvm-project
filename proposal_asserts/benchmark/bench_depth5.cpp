// 深度可扩展性测试：depth=5 的嵌套 generator（root->m1->m2->m3->leaf）
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <ranges>

#ifdef USE_IMPROVED
#include "generator_improved.hpp"
#else
#include "generator.hpp"
#endif

struct boom {};
static int sink = 0;

std::generator<int> leaf(int n) {
  for (int i = 0; i < n; ++i)
    co_yield i;
  throw boom{};
}

std::generator<int> m3(int n) {
  co_yield 0;
  co_yield std::ranges::elements_of(leaf(n));
  co_yield 1;
}
std::generator<int> m2(int n) {
  co_yield 0;
  co_yield std::ranges::elements_of(m3(n));
  co_yield 1;
}
std::generator<int> m1(int n) {
  co_yield 0;
  co_yield std::ranges::elements_of(m2(n));
  co_yield 1;
}
std::generator<int> root(int n) {
  co_yield 0;
  co_yield std::ranges::elements_of(m1(n));
  co_yield 1;
}

std::generator<int> leaf_ok(int n) {
  for (int i = 0; i < n; ++i)
    co_yield i;
}
std::generator<int> m3_ok(int n) {
  co_yield 0;
  co_yield std::ranges::elements_of(leaf_ok(n));
  co_yield 1;
}
std::generator<int> m2_ok(int n) {
  co_yield 0;
  co_yield std::ranges::elements_of(m3_ok(n));
  co_yield 1;
}
std::generator<int> m1_ok(int n) {
  co_yield 0;
  co_yield std::ranges::elements_of(m2_ok(n));
  co_yield 1;
}
std::generator<int> root_ok(int n) {
  co_yield 0;
  co_yield std::ranges::elements_of(m1_ok(n));
  co_yield 1;
}

static long long run_failing(int rounds) {
  int caught = 0;
  auto t0 = std::chrono::steady_clock::now();
  for (int r = 0; r < rounds; ++r) {
    try {
      auto g = root(3);
      for (int v : g) sink += v;
    } catch (const boom &) {
      ++caught;
    }
  }
  auto t1 = std::chrono::steady_clock::now();
  if (caught != rounds) { std::printf("FAIL: %d != %d\n", caught, rounds); std::exit(1); }
  return std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
}

static long long run_normal(int rounds) {
  auto t0 = std::chrono::steady_clock::now();
  for (int r = 0; r < rounds; ++r) {
    auto g = root_ok(3);
    for (int v : g) sink += v;
  }
  auto t1 = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
}

int main(int argc, char **argv) {
  int rounds = argc > 1 ? std::atoi(argv[1]) : 200000;
#ifdef USE_IMPROVED
  std::printf("impl: generator_improved.hpp (cancellation chain)\n");
#else
  std::printf("impl: generator.hpp (stock, rethrow per frame)\n");
#endif
  std::printf("depth=5 rounds=%d\n", rounds);
  long long n = run_normal(rounds);
  long long f = run_failing(rounds);
  std::printf("normal : %10lld ns total, %8.1f ns/round\n", n, (double)n / rounds);
  std::printf("failing: %10lld ns total, %8.1f ns/round\n", f, (double)f / rounds);
  std::printf("overhead per failing round: %8.1f ns\n", (double)(f - n) / rounds);
  std::printf("sink=%d\n", sink);
  return 0;
}
