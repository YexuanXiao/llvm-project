// RUN: %clang_cc1 -std=c++20 -triple=x86_64-unknown-linux-gnu -emit-llvm %s -o - -disable-llvm-passes | FileCheck %s
//
// A cancellation-aware coroutine (promise defines unhandled_cancellation):
//   - the frontend initializes the cancellation flag to false right after the
//     promise construction,
//   - every suspension point (the `*.ready` block, before `await_resume`)
//     checks the flag and branches to the `coro.cancel` block, which contains
//     the `promise.unhandled_cancellation()` call,
//   - the `__builtin_coro_request_cancel` / `__builtin_coro_cancel_requested`
//     builtins lower to `llvm.coro.promise` + GEP + store/load (no new LLVM
//     intrinsics; the flag byte is always reserved in the frame by LLVM).

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
// construction: store 0 to llvm.coro.promise(frame, alignof(promise), false)
// + sizeof(promise).
// CHECK: call ptr @llvm.coro.promise(ptr {{.*}}, i32 1, i1 false)
// CHECK: getelementptr inbounds i8, ptr {{.*}}, i64 1
// CHECK: store i8 0, ptr {{.*}}, align 1

// The cancellation check at the initial-suspend resumption point
// (`init.ready`, before `await_resume`): load the flag, branch to
// %coro.cancel when set.
// CHECK: init.ready:
// CHECK: call ptr @llvm.coro.promise(ptr {{.*}}, i32 1, i1 false)
// CHECK: getelementptr inbounds i8, ptr {{.*}}, i64 1
// CHECK: load i8, ptr {{.*}}, align 1
// CHECK: icmp ne i8 {{.*}}, 0
// CHECK: br i1 {{.*}}, label %coro.cancel, label %init.cancel.cont

// The cancellation entry block: unhandled_cancellation() + branch to the
// final suspend.
// CHECK: coro.cancel:
// CHECK: call void @_ZN4task12promise_type22unhandled_cancellationEv
// CHECK: br label %coro.final

// The final suspend is reached from both the body and the cancel block.
// CHECK: coro.final:

void request_cancel(std::coroutine_handle<task::promise_type> h) {
  h.request_cancel();
}

// CHECK-LABEL: define{{.*}} void @_Z14request_cancelSt16coroutine_handleIN4task12promise_typeEE(
// request_cancel stores 1 to the flag.
// CHECK: call ptr @llvm.coro.promise(ptr {{.*}}, i32 1, i1 false)
// CHECK: getelementptr inbounds i8, ptr {{.*}}, i64 1
// CHECK: store i8 1, ptr {{.*}}, align 1

bool cancel_requested(std::coroutine_handle<task::promise_type> h) {
  return h.cancel_requested();
}

// CHECK-LABEL: define{{.*}} zeroext i1 @_Z16cancel_requestedSt16coroutine_handleIN4task12promise_typeEE(
// cancel_requested loads the flag and compares it to zero.
// CHECK: call ptr @llvm.coro.promise(ptr {{.*}}, i32 1, i1 false)
// CHECK: getelementptr inbounds i8, ptr {{.*}}, i64 1
// CHECK: load i8, ptr {{.*}}, align 1
// CHECK: icmp ne i8 {{.*}}, 0
