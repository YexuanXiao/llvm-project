//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Benchmarks exception propagation through nested generators using the
// cooperative-cancellation chain (PxxxxR0): the exception is thrown once at
// the source and handled once at the root; intermediate frames only set the
// cancellation flag and transfer to the parent via symmetric transfer.
//
// REQUIRES the proposal-modified libc++ (coroutine_handle with
// request_cancel / cancel_requested / unhandled_cancellation) and the
// modified LLVM/Clang coroutine lowering; see proposal_asserts/build.md.
// The modified header is picked up through the normal libc++ include path
// (<__coroutine/coroutine_handle.h>), so no local copy is needed.

// UNSUPPORTED: c++03, c++11, c++14, c++17, c++20

#define IMPL_TAG "cancellation"
#include "generator_improved.hpp"

#include "coroutine_cancellation_bench.h"
