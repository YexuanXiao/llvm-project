//===- COMAddRefReleaseElim.h - COM AddRef/Release 成对消除
//----------------===//
//
// 消除冗余的 COM AddRef/Release 调用对。
// 适用于 C++/WinRT、ATL 等 COM 框架生成的 IUnknown 方法调用。
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_SCALAR_COMADDREFRELEASEELIM_H
#define LLVM_TRANSFORMS_SCALAR_COMADDREFRELEASEELIM_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class Function;

/// COM AddRef/Release 成对消除 pass。
///
/// 收集函数中 vtable[1] (AddRef) 和 vtable[2] (Release) 间接调用，
/// 安全配对并成对消除。
///
/// 安全约束（每一项均经过验证）：
///   1. 同一 source alloca（操作同一接口指针）
///   2. AddRef 支配 Release（确保 AddRef 先于 Release 执行）
///   3. 两者之间无 store 到该 alloca（排除对象替换）
///   4. 仅在同一基本块内配对（排除异常展开路径干扰）
///
/// 需要 DominatorTree 分析。必须在 !llvm.com_interfaces 元数据存在时才激活。
struct COMAddRefReleaseElimPass : PassInfoMixin<COMAddRefReleaseElimPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_SCALAR_COMADDREFRELEASEELIM_H
