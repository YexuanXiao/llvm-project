//===- COMAddRefReleaseElim.cpp - COM AddRef/Release 成对消除
//--------------===//
//
// 本文件是 LLVM 项目的一部分。
//
//===----------------------------------------------------------------------===//
//
// 设计理念
// ========
//
// C++/WinRT 等 COM 框架通过 IUnknown 虚函数表调度 AddRef 和 Release。
// com_ptr 的拷贝构造隐式调用 AddRef，析构隐式调用 Release。
// 经过内联后，函数内可能残留多个 AddRef/Release 对——它们操作同一接口指针，
// 引用计数净效果为零，可以成对消除。
//
// 例如:
//   com_ptr copy = original;  // AddRef (vtable[1])
//   copy.DoWork();
//   // copy 析构                     // Release (vtable[2])
//
// 内联到调用者后，AddRef 和 Release 操作同一 source alloca 上的接口指针。
// 本 Pass 收集所有 AddRef/Release，按 source alloca 分组，成对消除。
//
// 工作原理
// ========
//
// 关键洞察: AddRef 和 Release 操作的是同一接口指针，该指针存储在某个
// alloca 中。只要两条指令从同一个 alloca 加载接口指针，它们操作的就是
// 同一受控对象，引用计数的净效果为零。
//
// 算法分三步:
//
//   [遍历收集]
//   遍历函数中每条指令，对每个 CallInst:
//     - 若是 AddRef (vtable[1]):  记录 (source alloca → 此 call)
//     - 若是 Release (vtable[2]): 记录 (source alloca → 此 call)
//
//   [安全配对] 对每个 AddRef，在 Releases[alloca] 中寻找满足以下条件的 Release:
//
//     约束A — 同一基本块:
//       AddRef 和 Release 必须在同一 BB 内。
//       排除跨 BB 的支配缺失和异常展开路径问题。
//
//     约束B — 支配关系:
//       AddRef 必须支配 Release（DT.dominates），确保顺序正确。
//
//     约束C — 无 store 到 source alloca:
//       AddRef 和 Release 之间任何 store 写入该 alloca 都会
//       改变接口指针身份，使配对无效。
//
//   [成对消除] 满足所有约束则两两消除:
//     直接 eraseFromParent() 删除两条 call 指令。
//     AddRef/Release 返回 ULONG（新引用计数值），在 C++/WinRT
//     展开中此返回值从不被检查 (use_empty() == true)，删除调用安全。\n//\n//
//     不可消除的情况（残余保留）:\n//   - 跨 BB 的 AddRef/Release（Release
//     在异常展开路径等）\n//   - AddRef 和 Release 之间 store
//     了新的接口指针\n//   - AddRef 或 Release 的返回值被使用（违反 use_empty
//     断言）
//
// IUnknown 虚函数表布局:
//   vtable[0] = QueryInterface
//   vtable[1] = AddRef
//   vtable[2] = Release
//
// AddRef IR 模式:
//   MSVC ABI:  getelementptr i8, ptr %vtable, i64 8  → load → call
//   Itanium:   getelementptr ptr, ptr %vtable, i64 1 → load → call
//
// Release IR 模式:
//   MSVC ABI:  getelementptr i8, ptr %vtable, i64 16 → load → call
//   Itanium:   getelementptr ptr, ptr %vtable, i64 2  → load → call
//
// 安全守卫: Module 中存在 !llvm.com_interfaces 命名元数据
// （由 [[clang::com_iunknown]] 属性触发 Clang CodeGen 发出），
// 这确保了 vtable[1]/[2] 一定是 AddRef/Release，而非其他虚函数。
//
// 场景覆盖
// ========
//
// 场景1: COMQIMerge 插入的补偿 AddRef
//   COMQIMerge 合并 QI2→QI1 后插入 AddRef 补偿 QI2 的隐式 AddRef。
//   该 AddRef 与 com_ptr 析构的 Release 形成配对。
//   工作流: COMQIMerge → COMAddRefReleaseElim
//
// 场景2: 源码级 com_ptr 拷贝/析构对内联后的残留
//   com_ptr<T> copy = original;  // 隐式 AddRef (vtable[1])
//   ... 使用 copy ...
//   // copy 离开作用域          // 隐式 Release (vtable[2])
//   内联到调用者后，AddRef/Release 出现在同一函数中。
//   本 Pass 独立运行即可消除，无需 COMQIMerge 前置。
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/COMAddRefReleaseElim.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"
#include "llvm/Support/Casting.h"
#include "llvm/Transforms/Scalar.h"
#include <algorithm>

using namespace llvm;

#define DEBUG_TYPE "com-are-elim"

STATISTIC(NumAREPairsEliminated, "Number of AddRef/Release pairs eliminated");

/// 从指针追溯到其底层 alloca。
/// 处理 alloca、GEP (com_ptr::m_ptr)、BitCast、ConstantExpr 等中间指令。
static AllocaInst *getUnderlyingAlloca(Value *V) {
  SmallPtrSet<Value *, 8> Visited;
  while (V && Visited.insert(V).second) {
    if (auto *AI = dyn_cast<AllocaInst>(V))
      return AI;
    if (auto *GEP = dyn_cast<GetElementPtrInst>(V)) {
      V = GEP->getPointerOperand();
      continue;
    }
    if (auto *BC = dyn_cast<BitCastInst>(V)) {
      V = BC->getOperand(0);
      continue;
    }
    // ConstantExpr：全局常量 GEP/bitcast（极少见但需处理）
    if (auto *CE = dyn_cast<ConstantExpr>(V)) {
      if (CE->getOpcode() == Instruction::GetElementPtr ||
          CE->getOpcode() == Instruction::BitCast) {
        V = CE->getOperand(0);
        continue;
      }
    }
    return nullptr;
  }
  return nullptr;
}

/// 追踪一个 SSA 值回到 load 的源 alloca。声明在此，定义在后面。
static AllocaInst *traceToAlloca(Value *V, Instruction *StopBefore);

/// 识别 AddRef 调用（vtable[1] dispatch），追溯接口指针的 source alloca。
///
/// vtable[1] IR 模式（与 isReleaseCall 对称）：
///   MSVC ABI:  getelementptr i8, ptr %vtable, i64 8  (1 × 8 bytes)
///   Itanium:   getelementptr ptr, ptr %vtable, i64 1
static bool isAddRefCall(CallInst *CI, AllocaInst *&SourceAlloca) {
  // 安全检查：必须存在 COM 接口元数据且非空
  auto *COMInterfaces =
      CI->getModule()->getNamedMetadata("llvm.com_interfaces");
  if (!COMInterfaces || COMInterfaces->getNumOperands() == 0)
    return false;

  if (CI->getCalledFunction())
    return false;

  auto *FnLoad = dyn_cast<LoadInst>(CI->getCalledOperand());
  if (!FnLoad)
    return false;

  auto *G = dyn_cast<GetElementPtrInst>(FnLoad->getPointerOperand());
  if (!G || G->getNumIndices() != 1)
    return false;

  auto *Idx = dyn_cast<ConstantInt>(G->getOperand(1));
  if (!Idx)
    return false;

  uint64_t IdxVal = Idx->getZExtValue();
  if (G->getSourceElementType()->isIntegerTy(8)) {
    if (IdxVal != 8)
      return false; // MSVC: vtable[1] = 1*8 bytes
  } else {
    if (IdxVal != 1)
      return false; // Itanium: vtable[1]
  }

  // ABI 由 Sema::CheckCOMIUnknownRecord 在编译期保证，此处断言即可。
  assert(CI->arg_size() == 1 && "AddRef takes only 'this' parameter");
  assert(CI->getType()->isIntegerTy(32) &&
         "AddRef returns 32-bit unsigned integer");

  auto *VTableLoad = dyn_cast<LoadInst>(G->getPointerOperand());
  if (!VTableLoad)
    return false;

  // 接口指针 = AddRef 的 this 参数。追溯到其底层 alloca。
  Value *IfacePtr = VTableLoad->getPointerOperand();

  // 方法1: IfacePtr 直接是 load alloca (或 GEP→alloca) 的结果
  if (auto *IfaceLoad = dyn_cast<LoadInst>(IfacePtr)) {
    SourceAlloca = getUnderlyingAlloca(IfaceLoad->getPointerOperand());
    if (SourceAlloca)
      return true;
  }

  // 方法2: IfacePtr 是 SSA 值，向后追溯找 load 源
  SourceAlloca = traceToAlloca(IfacePtr, CI);
  return SourceAlloca != nullptr;
}

/// 追踪一个 SSA 值回到 load 的源 alloca。
/// 在同一个 BB 内向后扫描，找到定义该值的 LoadInst → alloca。
static AllocaInst *traceToAlloca(Value *V, Instruction *StopBefore) {
  // 直接是 load → alloca (或 GEP → alloca) 的情况
  if (auto *LI = dyn_cast<LoadInst>(V))
    return getUnderlyingAlloca(LI->getPointerOperand());

  // PHI 节点: 确保所有入边追踪到同一个 alloca，否则返回 null。
  if (auto *PHI = dyn_cast<PHINode>(V)) {
    AllocaInst *Common = nullptr;
    for (unsigned i = 0; i < PHI->getNumIncomingValues(); ++i) {
      AllocaInst *AI = traceToAlloca(PHI->getIncomingValue(i), StopBefore);
      if (!AI)
        return nullptr;
      if (!Common)
        Common = AI;
      else if (Common != AI)
        return nullptr; // 不同路径来自不同 alloca，不安全
    }
    return Common;
  }

  // 向后扫描: 找到存储在 V 中的 load 指令
  // (处理 store %v, ptr %alloca 后再 load 的模式)
  if (auto *I = dyn_cast<Instruction>(V)) {
    BasicBlock *BB = I->getParent();
    BasicBlock::iterator It(I);
    // 从 V 之前扫描
    while (It != BB->begin()) {
      --It;
      if (&*It == StopBefore)
        break;
      if (auto *SI = dyn_cast<StoreInst>(&*It)) {
        if (SI->getPointerOperand() == V ||
            (isa<GetElementPtrInst>(SI->getPointerOperand()) &&
             cast<GetElementPtrInst>(SI->getPointerOperand())
                     ->getPointerOperand() == V)) {
          if (auto *LI = dyn_cast<LoadInst>(SI->getValueOperand()))
            return getUnderlyingAlloca(LI->getPointerOperand());
        }
      }
    }
    return nullptr;
  }

  return nullptr;
}

/// 识别 Release (vtable[2] dispatch)，追溯接口指针的 source alloca。
static bool isReleaseCall(CallInst *CI, AllocaInst *&SourceAlloca) {
  auto *COMInterfaces =
      CI->getModule()->getNamedMetadata("llvm.com_interfaces");
  if (!COMInterfaces || COMInterfaces->getNumOperands() == 0)
    return false;

  if (CI->getCalledFunction())
    return false;

  auto *FnLoad = dyn_cast<LoadInst>(CI->getCalledOperand());
  if (!FnLoad)
    return false;

  auto *G = dyn_cast<GetElementPtrInst>(FnLoad->getPointerOperand());
  if (!G || G->getNumIndices() != 1)
    return false;

  auto *Idx = dyn_cast<ConstantInt>(G->getOperand(1));
  if (!Idx)
    return false;

  uint64_t IdxVal = Idx->getZExtValue();
  if (G->getSourceElementType()->isIntegerTy(8)) {
    if (IdxVal != 16)
      return false; // MSVC: vtable[2] = 2*8 bytes
  } else {
    if (IdxVal != 2)
      return false; // Itanium: vtable[2]
  }

  // ABI 由 Sema::CheckCOMIUnknownRecord 在编译期保证，此处断言即可。
  assert(CI->arg_size() == 1 && "Release takes only 'this' parameter");
  assert(CI->getType()->isIntegerTy(32) &&
         "Release returns 32-bit unsigned integer");

  auto *VTableLoad = dyn_cast<LoadInst>(G->getPointerOperand());
  if (!VTableLoad)
    return false;

  Value *IfacePtr = VTableLoad->getPointerOperand();

  // 方法1: IfacePtr 直接是 load alloca (或 GEP→alloca) 的结果
  if (auto *IfaceLoad = dyn_cast<LoadInst>(IfacePtr)) {
    SourceAlloca = getUnderlyingAlloca(IfaceLoad->getPointerOperand());
    if (SourceAlloca)
      return true;
  }

  // 方法2: IfacePtr 是 SSA 值，向后追溯找 load 源
  SourceAlloca = traceToAlloca(IfacePtr, CI);
  return SourceAlloca != nullptr;
}

PreservedAnalyses COMAddRefReleaseElimPass::run(Function &F,
                                                FunctionAnalysisManager &AM) {
  // 快速守卫：无 COM 接口元数据则跳过整个函数
  auto *COMInterfaces = F.getParent()->getNamedMetadata("llvm.com_interfaces");
  if (!COMInterfaces || COMInterfaces->getNumOperands() == 0)
    return PreservedAnalyses::all();

  auto &DT = AM.getResult<DominatorTreeAnalysis>(F);

  //=== 步骤1: 遍历收集 AddRef 和 Release 调用 ===
  //
  // 构建规范 alloca 映射: 若 alloca A 的值来自 store(load(alloca B))，
  // 则 A → B，确保 AddRef 和 Release 即使从不同 alloca 加载也能匹配。
  //
  // 重写策略：遍历所有 store 指令，而非遍历 alloca 的 user。
  // 因为通过 GEP 的 store（如 store ptr %v, ptr %m_ptr）是 GEP 的 user，
  // 不是 alloca 的直接 user，遍历 alloca->users() 会遗漏此类 store。
  DenseMap<AllocaInst *, StoreInst *> AllocaToStore;
  for (auto &BB : F) {
    for (auto &I : BB) {
      auto *SI = dyn_cast<StoreInst>(&I);
      if (!SI)
        continue;
      AllocaInst *Target = getUnderlyingAlloca(SI->getPointerOperand());
      if (!Target)
        continue;
      auto It = AllocaToStore.find(Target);
      if (It == AllocaToStore.end())
        AllocaToStore[Target] = SI;
      else
        AllocaToStore[Target] = nullptr; // 多个 store → 标记无效
    }
  }

  DenseMap<AllocaInst *, AllocaInst *> CanonicalAlloca;
  for (auto &KV : AllocaToStore) {
    if (!KV.second)
      continue; // nullptr 表示多个 store，无法确定唯一来源
    StoreInst *OnlyStore = KV.second;
    if (auto *LI = dyn_cast<LoadInst>(OnlyStore->getValueOperand()))
      if (auto *SrcAlloca = getUnderlyingAlloca(LI->getPointerOperand()))
        CanonicalAlloca[KV.first] = SrcAlloca;
  }

  // 闭合: 递归解析规范 alloca
  for (auto &KV : CanonicalAlloca) {
    AllocaInst *Cur = KV.second;
    SmallPtrSet<AllocaInst *, 8> Visited;
    while (CanonicalAlloca.count(Cur) && Visited.insert(Cur).second)
      Cur = CanonicalAlloca[Cur];
    KV.second = Cur;
  }

  // 按规范 alloca 分组 AddRef 和 Release
  // 同时也记录 Release 的原始 alloca，用于配对时的完整写入检查。
  auto getCanonical = [&](AllocaInst *AI) -> AllocaInst * {
    auto It = CanonicalAlloca.find(AI);
    return It != CanonicalAlloca.end() ? It->second : AI;
  };

  DenseMap<AllocaInst *, SmallVector<CallInst *, 4>> AddRefs;
  // Releases 映射: canonical alloca → vector of (ReleaseCall, OriginalAlloca)
  DenseMap<AllocaInst *, SmallVector<std::pair<CallInst *, AllocaInst *>, 4>>
      Releases;

  for (auto &BB : F) {
    for (auto &I : BB) {
      auto *CI = dyn_cast<CallInst>(&I);
      if (!CI)
        continue;
      AllocaInst *SrcAlloca = nullptr;
      if (isAddRefCall(CI, SrcAlloca)) {
        LLVM_DEBUG(dbgs() << "COMARE: AddRef call " << *CI << " → alloca "
                          << *SrcAlloca
                          << " canonical=" << *getCanonical(SrcAlloca) << "\n");
        AddRefs[getCanonical(SrcAlloca)].push_back(CI);
      } else if (isReleaseCall(CI, SrcAlloca)) {
        LLVM_DEBUG(dbgs() << "COMARE: Release call " << *CI << " → alloca "
                          << *SrcAlloca
                          << " canonical=" << *getCanonical(SrcAlloca) << "\n");
        Releases[getCanonical(SrcAlloca)].push_back({CI, SrcAlloca});
      }
    }
  }

  if (AddRefs.empty())
    return PreservedAnalyses::all();

  //=== 步骤2: 安全配对并消除 ===
  //
  // 对每个 AddRef，在同 source alloca 的 Release 列表中寻找安全配对。
  //
  // 安全约束：
  //   A. 支配关系: AddRef 必须支配 Release。
  //      确保 AddRef 在每条到达 Release 的路径上都先执行。
  //      自动排除异常展开路径 (ehcleanup 不被正常 BB 支配)。
  //   B. 无 store 写入: AddRef→Release (沿 IDom 链) 之间不能有
  //      store 写入 source alloca，防止接口指针身份改变。
  //
  // 配对策略：
  //   对每个 AddRef，在 Releases[alloca] 中寻找第一个满足支配+无 store
  //   约束的 Release。找到则消除，该 Release 标记已使用。
  bool Changed = false;

  // 辅助函数：穿透 GEP/BitCast/ConstantExpr 追溯到 alloca（同
  // getUnderlyingAlloca） 保留此别名以保持代码自文档化：调用指令写入检查。

  // 辅助函数：检查指令是否会写入指定 alloca（包含 store 和调用）
  auto instructionWritesToAlloca = [&](Instruction &I,
                                       AllocaInst *CheckAlloca) -> bool {
    // Store 指令：任何对 CheckAlloca 的写入都改变接口指针身份。
    if (auto *SI = dyn_cast<StoreInst>(&I)) {
      if (getUnderlyingAlloca(SI->getPointerOperand()) == CheckAlloca)
        return true;
      return false;
    }
    // 调用指令：若传入 alloca 作为参数且可能写入，视为覆盖
    if (auto *CI = dyn_cast<CallInst>(&I)) {
      // 跳过不修改内存内容的 intrinsic（lifetime.start/end）
      if (auto *II = dyn_cast<IntrinsicInst>(CI)) {
        auto ID = II->getIntrinsicID();
        if (ID == Intrinsic::lifetime_start || ID == Intrinsic::lifetime_end)
          return false;
      }
      // readnone/readonly 调用：不写入内存
      if (CI->doesNotAccessMemory() || CI->onlyReadsMemory())
        return false;
      // 检查是否将 alloca 或 GEP 传入调用（可能是输出参数）
      for (auto &Arg : CI->args())
        if (getUnderlyingAlloca(Arg.get()) == CheckAlloca)
          return true;
    }
    return false;
  };

  // 辅助函数：检查 AddRef→Release 之间的全区域是否有指令写入 alloca。
  // 使用逆向 BFS 遍历 CFG，覆盖从 AddRef 到 Release 的所有可能路径，
  // 而非仅沿 IDom 链（后者会遗漏菱形 CFG 中的中间块）。
  // 算法与 COMQIMerge::safeToMerge 的跨 BB 干扰检查保持一致。
  auto hasInterveningStore = [&](CallInst *AddRef, CallInst *Release,
                                 AllocaInst *CheckAlloca) -> bool {
    BasicBlock *ARBB = AddRef->getParent();
    BasicBlock *RelBB = Release->getParent();

    // 同一 BB: 扫描 AddRef 之后、Release 之前的指令
    if (ARBB == RelBB) {
      BasicBlock::iterator I(AddRef);
      BasicBlock::iterator End(Release);
      for (++I; I != End; ++I)
        if (instructionWritesToAlloca(*I, CheckAlloca))
          return true;
      return false;
    }

    // 跨 BB：收集从 AddRef 到 Release 的全区域 BB。
    // 从 RelBB 逆向 BFS 遍历 CFG，访问所有被 ARBB 支配的前驱块。
    SmallPtrSet<BasicBlock *, 16> Visited;
    SmallVector<BasicBlock *, 16> WorkList;
    SmallVector<BasicBlock *, 16> RegionBBs;
    Visited.insert(RelBB);
    WorkList.push_back(RelBB);

    while (!WorkList.empty()) {
      BasicBlock *BB = WorkList.pop_back_val();
      for (auto *Pred : predecessors(BB)) {
        if (!Visited.insert(Pred).second)
          continue;
        // 只访问被 ARBB 支配的块 — 这些块保证在 AddRef 之后执行
        if (!DT.dominates(ARBB, Pred))
          continue;
        if (Pred == ARBB)
          continue; // ARBB 单独处理
        RegionBBs.push_back(Pred);
        WorkList.push_back(Pred);
      }
    }

    // 检查 ARBB 中 AddRef 之后的所有指令
    bool PastAddRef = false;
    for (Instruction &I : *ARBB) {
      if (&I == AddRef) {
        PastAddRef = true;
        continue;
      }
      if (!PastAddRef)
        continue;
      if (instructionWritesToAlloca(I, CheckAlloca))
        return true;
    }

    // 检查所有中间基本块的全部指令
    for (BasicBlock *BB : RegionBBs) {
      for (Instruction &I : *BB)
        if (instructionWritesToAlloca(I, CheckAlloca))
          return true;
    }

    // 检查 RelBB 中 Release 之前的指令
    for (Instruction &I : *RelBB) {
      if (&I == Release)
        break;
      if (instructionWritesToAlloca(I, CheckAlloca))
        return true;
    }
    return false;
  };

  //=== 主要配对逻辑 ===
  for (auto &KV : AddRefs) {
    AllocaInst *Alloca = KV.first;
    auto &AddRefVec = KV.second;
    auto It = Releases.find(Alloca);
    if (It == Releases.end()) {
      LLVM_DEBUG(dbgs() << "COMARE: No Release for alloca " << *Alloca
                        << " (AddRefs=" << AddRefVec.size() << ")\n");
      continue;
    }
    auto &ReleaseVec = It->second;

    for (CallInst *AddRefCall : AddRefVec) {
      // 在 ReleaseVec 中寻找安全配对
      for (auto RI = ReleaseVec.begin(); RI != ReleaseVec.end(); ++RI) {
        CallInst *ReleaseCall = RI->first;
        AllocaInst *ReleaseOrigAlloca = RI->second;

        // 约束A: 支配关系
        if (!DT.dominates(AddRefCall, ReleaseCall))
          continue;

        // 约束A2: 同一 EH funclet 上下文
        // AddRef 和 Release 必须在同一个 funclet 中（或都不在 funclet 中）。
        // Windows EH 将函数拆分为多个 funclet 区域，跨 funclet 配对会导致
        // 正常路径的 AddRef 被异常路径的 Release 抵消，反之亦然，
        // 最终引用计数错乱。
        {
          auto ARBundle = AddRefCall->getOperandBundle("funclet");
          auto RelBundle = ReleaseCall->getOperandBundle("funclet");
          if (ARBundle || RelBundle) {
            // 一方在 funclet 中，另一方不在 → 拒绝
            if (!ARBundle || !RelBundle)
              continue;
            // 两方都在 funclets 中，但 token 不同 → 拒绝
            if (ARBundle->Inputs[0] != RelBundle->Inputs[0])
              continue;
          }
        }

        // 约束B: 无写入到 canonical alloca 或 Release 的原始 alloca
        // 构建相关 alloca 集合：规范 alloca + Release 原始 alloca
        SmallPtrSet<AllocaInst *, 4> RelatedAllocas;
        RelatedAllocas.insert(Alloca);
        if (ReleaseOrigAlloca != Alloca)
          RelatedAllocas.insert(ReleaseOrigAlloca);

        bool HasInterference = false;
        for (auto *CheckA : RelatedAllocas) {
          if (hasInterveningStore(AddRefCall, ReleaseCall, CheckA)) {
            HasInterference = true;
            break;
          }
        }
        if (HasInterference)
          continue;

        // 安全：可以消除
        assert(AddRefCall->use_empty() && "AddRef call has users");
        assert(ReleaseCall->use_empty() && "Release call has users");
        AddRefCall->eraseFromParent();
        ReleaseCall->eraseFromParent();
        ++NumAREPairsEliminated;
        Changed = true;

        ReleaseVec.erase(RI);
        break;
      }
    }
  }

  if (!Changed)
    return PreservedAnalyses::all();
  // none(): 修改了函数体（删除了调用指令），不保留任何分析结果
  return PreservedAnalyses::none();
}
