// RUN: %clang_cc1 -std=c++20 -triple=x86_64-unknown-linux-gnu -emit-llvm %s -o - -disable-llvm-passes | FileCheck %s
//
// A cancellation-aware coroutine (promise defines unhandled_cancellation):
//   - the frontend initializes the cancellation flag to false right after the
//     promise construction,
//   - every suspension point (the `*.ready` block, before `await_resume`)
//     checks the flag (via `llvm.coro.cancel.requested`) and branches to the
//     `coro.cancel` block, which contains the
//     `promise.unhandled_cancellation()` call,
//   - the `__builtin_coro_request_cancel` / `__builtin_coro_cancel_requested`
//     builtins lower to `llvm.coro.request.cancel` /
//     `llvm.coro.cancel.requested`, which CoroEarly lowers into plain pointer
//     arithmetic at the fixed frame offset (no promise-type information
//     needed);
//   - the cancellation members are unconstrained: they exist on every handle
//     (including `coroutine_handle<void>`); calling them on a coroutine that
//     is not cancellation-aware is undefined behavior,
//   - `promise()`/`from_promise()` use the shifted projection
//     `alignTo(2*sizeof(void*) + 1, alignof(Promise))` for cancellation-aware
//     promises.

#include "Inputs/coroutine.h"

struct task {
  struct promise_type {
    bool cancelled = false;
    task get_return_object();
    std::suspend_always initial_suspend();
    std::suspend_always final_suspend() noexcept;
    void return_void();
    void unhandled_exception();
    void unhandled_cancellation() { cancelled = true; }
  };
  std::coroutine_handle<promise_type> h;
  using handle = std::coroutine_handle<promise_type>;
  task(handle h) : h(h) {}
};

task cancel_me() {
  co_await std::suspend_always{};
}

// CHECK-LABEL: define{{.*}} ptr @_Z9cancel_mev(
// The cancellation flag is initialized to false right after the promise
// construction: store 0 to frame + 2 * sizeof(void*) (the fixed offset
// immediately after the resume/destroy function pointers, before the
// promise).
// CHECK: getelementptr inbounds i8, ptr {{.*}}, i64 16
// CHECK: store i8 0, ptr {{.*}}, align 1

// The cancellation check at the initial-suspend resumption point
// (`init.ready`, before `await_resume`): query the flag at the fixed frame
// offset via llvm.coro.cancel.requested, branch to %coro.cancel when set.
// CHECK: init.ready:
// CHECK: call i1 @llvm.coro.cancel.requested(ptr %{{.*}})
// CHECK: br i1 %{{.*}}, label %coro.cancel, label %init.cancel.cont

// The cancellation entry block: marker + unhandled_cancellation() + branch
// to the final suspend.
// CHECK: coro.cancel:
// CHECK-NEXT: call void @llvm.coro.cancelled()
// CHECK: call void @_ZN4task12promise_type22unhandled_cancellationEv
// CHECK: br label %coro.final

// The final suspend is reached from both the body and the cancel block.
// CHECK: coro.final:

void request_cancel(std::coroutine_handle<task::promise_type> h) {
  h.request_cancel();
}

// CHECK-LABEL: define{{.*}} void @_Z14request_cancelSt16coroutine_handleIN4task12promise_typeEE(
// request_cancel lowers to llvm.coro.request.cancel (CoroEarly turns it into
// a store of 1 to the flag at the fixed offset 16).
// CHECK: call void @llvm.coro.request.cancel(ptr %{{.*}})

bool cancel_requested(std::coroutine_handle<task::promise_type> h) {
  return h.cancel_requested();
}

// CHECK-LABEL: define{{.*}} zeroext i1 @_Z16cancel_requestedSt16coroutine_handleIN4task12promise_typeEE(
// cancel_requested lowers to llvm.coro.cancel.requested (CoroEarly turns it
// into a load of the flag and a comparison).
// CHECK: call i1 @llvm.coro.cancel.requested(ptr %{{.*}})

// The cancellation members are unconstrained and exist on the type-erased
// handle too: locating the flag never requires promise-type information.
void request_cancel_erased(std::coroutine_handle<> h) {
  h.request_cancel();
}

// CHECK-LABEL: define{{.*}} void @_Z21request_cancel_erasedSt16coroutine_handleIvE(
// CHECK: call void @llvm.coro.request.cancel(ptr %{{.*}})

bool cancel_requested_erased(std::coroutine_handle<> h) {
  return h.cancel_requested();
}

// CHECK-LABEL: define{{.*}} zeroext i1 @_Z23cancel_requested_erasedSt16coroutine_handleIvE(
// CHECK: call i1 @llvm.coro.cancel.requested(ptr %{{.*}})

// Promise access on a cancellation-aware promise uses the shifted projection
// alignTo(2*sizeof(void*) + 1, alignof(Promise)) = alignTo(17, 1) = 17,
// instead of the plain `llvm.coro.promise` formula (which assumes the flag
// byte is absent).
task::promise_type &get_promise(std::coroutine_handle<task::promise_type> h) {
  return h.promise();
}

// CHECK-LABEL: define{{.*}} ptr @_Z11get_promiseSt16coroutine_handleIN4task12promise_typeEE(
// CHECK: getelementptr inbounds {{(nuw )?}}i8, ptr %{{.*}}, i64 17

std::coroutine_handle<task::promise_type> from_promise(task::promise_type &p) {
  return std::coroutine_handle<task::promise_type>::from_promise(p);
}

// CHECK-LABEL: define{{.*}} ptr @_Z12from_promiseRN4task12promise_typeE(
// CHECK: getelementptr inbounds {{(nuw )?}}i8, ptr %{{.*}}, i64 -17