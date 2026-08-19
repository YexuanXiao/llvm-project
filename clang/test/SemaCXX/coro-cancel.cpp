// RUN: %clang_cc1 -std=c++20 -triple=x86_64-unknown-linux-gnu -fsyntax-only -verify %s
//
// Cooperative cancellation (PxxxxR0):
//   - `unhandled_cancellation` is optional: a coroutine whose promise lacks it
//     compiles as before (cancellation simply has no effect at the language
//     level),
//   - a coroutine whose promise declares it compiles too,
//   - the `__builtin_coro_request_cancel` / `__builtin_coro_cancel_requested`
//     builtins are available,
//   - `request_cancel()` / `cancel_requested()` are unconstrained members of
//     every `coroutine_handle` (including `coroutine_handle<void>` and
//     handles of non-cancellation-aware coroutines); calling them on a
//     coroutine that is not cancellation-aware is undefined behavior.

#include "Inputs/std-coroutine.h"

// A cancellation-aware promise.
struct task {
  struct promise_type {
    bool cancelled = false;
    task get_return_object();
    std::suspend_always initial_suspend();
    std::suspend_always final_suspend() noexcept;
    void return_void() {}
    void unhandled_exception() {}
    void unhandled_cancellation() { cancelled = true; }
  };
  std::coroutine_handle<promise_type> h;
  task(std::coroutine_handle<promise_type> h) : h(h) {}
};

task f1() { // expected-no-diagnostics
  co_await std::suspend_always{};
}

// A promise without unhandled_cancellation: still a valid coroutine.
struct plain {
  struct promise_type {
    plain get_return_object();
    std::suspend_always initial_suspend();
    std::suspend_always final_suspend() noexcept;
    void return_void() {}
    void unhandled_exception() {}
  };
  std::coroutine_handle<promise_type> h;
  plain(std::coroutine_handle<promise_type> h) : h(h) {}
};

plain f2() { // expected-no-diagnostics
  co_await std::suspend_always{};
}

// The builtins are usable directly on a frame handle; the cancellation flag
// lives at the fixed offset 2 * sizeof(void*) in the frame.
void builtins(void *handle) {
  __builtin_coro_request_cancel(handle);
  bool b = __builtin_coro_cancel_requested(handle);
  (void)b;
}

// The cancellation members are unconstrained: they exist on every handle,
// including handles of non-cancellation-aware coroutines and the type-erased
// handle. Calling them on a coroutine that is not cancellation-aware is
// undefined behavior (the caller's responsibility).
static_assert(requires(std::coroutine_handle<plain::promise_type> h) {
  h.request_cancel();
  h.cancel_requested();
});
static_assert(requires(std::coroutine_handle<> h) {
  h.request_cancel();
  h.cancel_requested();
});
