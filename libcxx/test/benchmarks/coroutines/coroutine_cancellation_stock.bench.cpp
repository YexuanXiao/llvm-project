//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Benchmarks exception propagation through nested generators with the stock
// std::generator behavior (one throw/catch pair per nested frame). This is
// the baseline that the cancellation chain in
// coroutine_cancellation.bench.cpp is compared against; both are compiled
// against the same (proposal-modified) libc++.

// UNSUPPORTED: c++03, c++11, c++14, c++17, c++20

#define IMPL_TAG "stock"
#include "generator.hpp"

#include "coroutine_cancellation_bench.h"
