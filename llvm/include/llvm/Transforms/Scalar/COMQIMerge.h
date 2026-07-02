//===- COMQIMerge.h - COM QueryInterface call merging -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// This file provides the interface for a pass that merges redundant COM
/// QueryInterface calls. When C++/WinRT generates code that calls
/// QueryInterface through the vtable on the same object with the same IID,
/// this pass eliminates the redundant calls by reusing the result of the
/// first call.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_SCALAR_COMQIMERGE_H
#define LLVM_TRANSFORMS_SCALAR_COMQIMERGE_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class Function;

/// A pass that merges redundant COM QueryInterface calls.
///
/// This pass walks the dominator tree to find calls to QueryInterface
/// (via vtable dispatch) on the same object with the same IID, and
/// replaces redundant calls with the result of the first call.
struct COMQIMergePass : PassInfoMixin<COMQIMergePass> {
  /// Run the pass over the function.
  LLVM_ABI PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_SCALAR_COMQIMERGE_H
