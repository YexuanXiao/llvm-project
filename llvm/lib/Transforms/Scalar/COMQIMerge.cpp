//===- COMQIMerge.cpp - COM QueryInterface 调用合并优化
//--------------------===//
//
// 本文件是 LLVM 项目的一部分。
//
//===----------------------------------------------------------------------===//
//
// 设计理念
// ========
//
// C++/WinRT 是一个仅头文件的 C++ COM 运行时库。每次调用 COM 接口方法时，
// 它通过 consume_general 模板 -> try_as_with_reason -> QueryInterface(IID,
// &result)
// 来获取接口指针。若连续对同一对象调用同一接口的多个方法，就会产生多个相同
// IID 的 QueryInterface 调用，每个调用都返回相同的接口指针 — 这正是本 Pass
// 要消除的冗余。
//
// 示例（MSVC 风格的 C++/WinRT 展开）：
//   obj.DoWork();   // 隐藏调用 QueryInterface(IID_IFoo, &p1)
//   obj.DoExtra();  // 隐藏调用 QueryInterface(IID_IFoo, &p2) ← 冗余!
//   obj.DoMore();   // 隐藏调用 QueryInterface(IID_IFoo, &p3) ← 冗余!
// 优化后：只保留第一个 QueryInterface，后续调用复用其结果。
//
// 识别机制
// ========
//
// Clang 的 [[clang::com_iunknown]] 属性标记 COM 接口 ABI 结构体（即包含
// QueryInterface/AddRef/Release 的虚函数表结构体）。Clang CodeGen 在看到此
// 属性时向模块发出 !llvm.com_interfaces 命名元数据。本 Pass 仅在元数据存在时
// 才激活 — 这确保了安全性：vtable[0] 一定是 QueryInterface。
//
// IUnknown 虚函数表布局：
//   vtable[0] = QueryInterface
//   vtable[1] = AddRef
//   vtable[2] = Release
//
// 两种 vtable 调度 IR 模式（均被识别）：
//
//   模式A（优化前，带显式 GEP）：
//     %vtable = load ptr, ptr %this
//     %vfn    = getelementptr ptr, ptr %vtable, i64 0  ; vtable[0]
//     %fn     = load ptr, ptr %vfn
//     %hr     = call i32 %fn(ptr %this, ptr @IID, ptr %result)
//
//   模式B（优化后，GEP i64 0 被常量折叠为 no-op）：
//     %vtable = load ptr, ptr %this
//     %fn     = load ptr, ptr %vtable              ; 直接从 vtable 加载
//     %hr     = call i32 %fn(ptr %this, ptr @IID, ptr %result)
//
// 安全检查
// ========
//
// 本 Pass 必须保守以确保正确性，主要处理以下危险场景：
//
// 1. Release() 干扰：两次 QI 之间若同一对象被 Release，接口指针可能失效，
//    合并将导致悬垂指针。通过检测 vtable[2] 的间接调用（Release 特征）来捕获。
//
// 2. 对象替换：alloca 被重新赋值（store ptr %newObj, ptr %this_alloca），
//    导致 this 指针指向不同对象。通过追踪 store 指令来捕获。
//
// 3. 循环迭代：循环内同一 alloca 可能在不同迭代中保存不同对象。由于
//    支配树前序遍历保证每个 BB 只访问一次，跨迭代合并不会发生。但循环头
//    进入时清除循环体内部的条目，防止循环展开后的误匹配。
//
// 4. 结果覆盖：第一个 QI 的 result alloca 在第二个 QI 之前被写入，
//    导致结果内容被破坏。通过追踪 store 指令来捕获。
//
// 合并策略
// ========
//
// 当 QI2 判定为安全可合并时：
//   1. 在 First QI 之后插入 AddRef (vtable[1])，补偿 QI2 的隐式 AddRef
//   2. 若两 QI 写入不同 result alloca，复制接口指针到 Second 的 alloca
//   3. RAUW + 删除 Second 的 CallInst
//
//   QI 内部 AddRef 返回的接口指针，com_ptr 析构时 Release。
//   合并 QI2→QI1 消除 QI2 后，QI2 对应的 Release 仍存在，
//   必须在 QI1 之后插入 AddRef(+1) 来平衡。
//
//   合并前: QI1(+1)→Rel(-1)→QI2(+1)→Rel(-1)  = 净0
//   合并后: QI1(+1)→AddRef(+1)→Rel(-1)→Rel(-1) = 净0 ✓
//
//   后续由 COMAddRefReleaseElimPass 消除 AddRef/Release 对。
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/COMQIMerge.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/Local.h"

using namespace llvm;

#define DEBUG_TYPE "com-qi-merge"

// 统计计数器：成功合并的 QueryInterface 调用数量
STATISTIC(NumQIMerged, "Number of QueryInterface calls merged");

namespace {

/// 识别到的 QueryInterface 调用点。
///
/// 封装一次 QI 间接调用所需的所有 IR 值，便于后续的相等性比较和合并操作。
struct QICall {
  /// QI 间接调用指令本身: call i32 %fn(ptr %this, ptr @IID, ptr %result)
  CallInst *Call;

  /// QI 调用的第0参数：this 指针（COM 对象地址）
  /// 合并依据之一：同一对象的 QI 结果才是可互换的。
  Value *ObjectPtr;

  /// QI 调用的第1参数：IID/GUID 指针（接口唯一标识符）
  /// 合并依据之二：同一 IID 的查询才返回相同接口指针。
  Value *GUIDPtr;

  /// QI 调用的第2参数：输出指针（QI 将接口指针写入此处）
  Value *ResultPtr;

  QICall()
      : Call(nullptr), ObjectPtr(nullptr), GUIDPtr(nullptr),
        ResultPtr(nullptr) {}
};

/// 可用 QI 调用的查找键：(对象, IID) 二元组。
///
/// 当两个 QI 调用的 ObjectPtr 和 GUIDPtr 穿透到底层都指向同一来源时，
/// 它们的 QI 结果是等价的，可以合并。
/// 使用 std::pair 而非自定义结构体，因为 LLVM 为 std::pair<T,U> 提供了
/// 开箱即用的 DenseMapInfo 特化。
using QIKey = std::pair<Value *, Value *>;

} // anonymous namespace

/// 穿透不改变指针标识的 IR 指令，追溯到其底层来源。
///
/// 在函数内联和 mem2reg（将 alloca 提升为 SSA 寄存器）之后，同一次 QI 调用
/// 的 this 指针可能产生多个 SSA 值（例如不同的 load 指令从同一个 alloca
/// 加载）。 本函数剥离以下不影响指针标识的指令，以找到"根"值：
///
///   - LoadInst：          %v2 = load ptr, ptr %p  → 追踪到 %p
///   - GEP (全零索引)：    %v2 = getelementptr ptr, ptr %v1, i64 0, i64 0 →
///   追踪到 %v1
///   - BitCastInst：       %v2 = bitcast ptr %v1 to ptr → 追踪到 %v1
///
/// 循环安全：Visited 集合确保在遇到指针自循环（理论上不应出现）时终止。
///
/// \param V 起始值
/// \returns 穿透上述指令后的底层值（通常是 alloca 或函数参数）
static Value *getUnderlyingObject(Value *V) {
  // 防止无限循环的访问记录
  SmallPtrSet<Value *, 8> Visited;
  while (true) {
    if (!Visited.insert(V).second)
      break;
    // Load 指令：从指针加载值，穿透到指针本身
    if (auto *LI = dyn_cast<LoadInst>(V)) {
      V = LI->getPointerOperand();
      continue;
    }
    // 零偏移 GEP：地址未变化（常见于 MSVC ABI 中 mem2reg 后对 this 的调整）
    if (auto *GEP = dyn_cast<GetElementPtrInst>(V)) {
      if (GEP->hasAllZeroIndices()) {
        V = GEP->getPointerOperand();
        continue;
      }
    }
    // BitCast：指针类型转换不改变地址
    if (auto *BC = dyn_cast<BitCastInst>(V)) {
      V = BC->getOperand(0);
      continue;
    }
    break;
  }
  return V;
}

/// 识别一个 CallInst 是否是通过 vtable[0] 调用的 COM QueryInterface。
///
/// 识别流程（按顺序的短路检查，越早失败成本越低）：
///
///   1. 安全检查：模块必须包含 !llvm.com_interfaces 命名元数据。
///      此元数据由 Clang CodeGen 在看到 [[clang::com_iunknown]] 属性时发出，
///      是该模块存在 COM 接口的权威信号。若无此元数据，直接拒绝整个模块
///      的所有候选调用 —— 绝不猜测。
///
///   2. 签名检查：间接调用、恰好3个参数、返回 i32 (HRESULT)。
///
///   3. 参数类型检查：三个参数必须全是指针类型。
///
///   4. 调用目标检查：被调用者必须通过 LoadInst 获取（vtable 查找）。
///
///   5. vtable[0] 路径验证（两种模式）：
///      - 模式A：被调用者通过 GEP i64 0 -> Load 获取（vtable[0] 显式索引）
///      - 模式B：被调用者直接从 vtable 加载（GEP 已被优化折叠）
///
///   6. this 一致性：vtable 加载的来源（穿透后）必须等于 QI 的第0参数。
///      若不相等，说明被调用者来自另一个对象的 vtable。
///
///   7. 结果 alloca 提取：从第2参数穿透找到底层 alloca，供后续合并使用。
///
/// \param CI 待检查的调用指令
/// \param[out] QI 若识别成功，填充此结构体
/// \returns 成功识别为 true
static bool recognizeQueryInterface(CallInst *CI, QICall &QI) {
  // 第1关：模块级守卫。Clang 遇到 [[clang::com_iunknown]] 时发出此元数据。
  // 没有它，我们无法安全地区分 QueryInterface 调用和普通的间接调用。
  if (!CI->getModule()->getNamedMetadata("llvm.com_interfaces") ||
      CI->getModule()
              ->getNamedMetadata("llvm.com_interfaces")
              ->getNumOperands() == 0)
    return false;

  // 第2关：必须是间接调用（虚函数通过 vtable 调度，无直接被调用者）
  if (CI->getCalledFunction())
    return false;

  // 第2关（续）：QueryInterface 有三参数签名 — this, IID, result
  //   ABI 由 Sema::CheckCOMIUnknownRecord 在编译期保证，此处断言即可。
  assert(CI->arg_size() == 3 && "QI must have 3 args (this, iid, result)");

  // 第2关（续）：COM 方法返回 HRESULT，即 i32
  assert(CI->getType()->isIntegerTy(32) && "QI must return 32-bit integer");

  // 第3关：三个参数都必须是指针类型
  assert(CI->getArgOperand(0)->getType()->isPointerTy() &&
         "this must be pointer");
  assert(CI->getArgOperand(1)->getType()->isPointerTy() &&
         "iid must be pointer");
  assert(CI->getArgOperand(2)->getType()->isPointerTy() &&
         "result must be pointer");

  // 第4关：被调用者必须通过 LoadInst 获取（vtable 查找的特征）
  Value *Callee = CI->getCalledOperand();
  auto *Load = dyn_cast<LoadInst>(Callee);
  if (!Load)
    return false;

  // 第5关：验证 vtable[0] 路径
  //
  // 被调用者来自：
  //   模式A: load(GEP(vtable, 0)) — GEP 显式索引到 vtable[0]
  //   模式B: load(vtable)         — O2 已将 GEP 0 折叠为 no-op
  //
  // 在两种模式中，vtable 本身是从 this 指针加载的。
  Value *VFNPtr = Load->getPointerOperand();
  LoadInst *VTableLoad = nullptr;

  if (auto *GEP = dyn_cast<GetElementPtrInst>(VFNPtr)) {
    // 模式A: %vfn = getelementptr ptr, ptr %vtable, i64 0
    if (GEP->getNumIndices() != 1)
      return false;
    auto *Idx = dyn_cast<ConstantInt>(GEP->getOperand(1));
    if (!Idx || !Idx->isZero())
      return false;
    // GEP 的基础指针必须是从 this 加载的 vtable
    Value *VTable = GEP->getPointerOperand();
    VTableLoad = dyn_cast<LoadInst>(VTable);
    if (!VTableLoad)
      return false;
  } else {
    // 模式B: %fn = load ptr, ptr %vtable （GEP 0 已被折叠）
    VTableLoad = dyn_cast<LoadInst>(VFNPtr);
    if (!VTableLoad)
      return false;
  }

  // 第6关：vtable 加载的来源 == QI 调用的 this 参数
  //
  // 使用 getUnderlyingObject 剥离中间的 load/alloca 访问链，
  // 以处理内联和 mem2reg 产生的不同 SSA 值。
  Value *ThisArg = CI->getArgOperand(0);
  Value *VTableThis = VTableLoad->getPointerOperand();
  if (getUnderlyingObject(VTableThis) != getUnderlyingObject(ThisArg))
    return false;

  // 识别完成。
  // [[clang::com_iunknown]] 属性（由 !llvm.com_interfaces 元数据表征）
  // 保证 vtable[0] 就是 QueryInterface，无需再检查 GUID 结构体。
  QI.Call = CI;
  QI.ObjectPtr = CI->getArgOperand(0);
  QI.GUIDPtr = CI->getArgOperand(1);
  QI.ResultPtr = CI->getArgOperand(2);

  return true;
}

/// 检查两条 QI 调用之间的某条指令是否会阻止合并。
///
/// 这是合并安全分析的核心：逐条检查 First 之后、Second 之前的每条指令，
/// 判断它是否会破坏"第二条 QI 的返回值可以用第一条的返回值替代"这个不变量。
///
/// 返回值语义：true = 合并被阻止（指令干扰），false = 合并安全继续。
///
/// \param I 待检查的指令
/// \param First 第一条（先执行的）QI 调用
/// \param Second 第二条（后执行的）QI 调用
/// \returns true 表示存在干扰，不应合并
static bool instructionInterferes(Instruction *I, const QICall &First,
                                  const QICall &Second) {
  // lifetime.start / lifetime.end 不影响 QI 结果的正确性，
  // 因为它们只是给优化器的提示，不改变内存内容。
  if (isa<IntrinsicInst>(I)) {
    auto *II = cast<IntrinsicInst>(I);
    if (II->getIntrinsicID() == Intrinsic::lifetime_start ||
        II->getIntrinsicID() == Intrinsic::lifetime_end)
      return false;
  }

  // 调用指令分析：
  // 关键危险: (1) Release() 销毁接口指针; (2) 任何间接调用可能写入输出参数。
  if (auto *CI = dyn_cast<CallInst>(I)) {
    // 跳过我们正在比对的 QI 调用自身
    if (CI == First.Call || CI == Second.Call)
      return false;

    //=== 检查调用是否会写入 First 或 Second 的结果存储位置 ===
    //
    // 间接调用（或非只读直接调用）可能通过指针参数写入内存。
    // 若某个参数是 First.ResultPtr 或 Second.ResultPtr（或其底层 alloca），
    // 调用可能覆盖 QI 结果 → 阻止合并。
    //
    // 区分策略：若参数是 LoadInst，调用接收的是加载后的值（接口指针本身），
    // 而非存储位置的地址，此时调用无法写回我们的局部变量。
    // 若参数是 alloca / GEP / bitcast（非 LoadInst），则调用接收的是
    // 存储位置的地址，可以写入 → 干扰。
    Value *FirstUnderlying = getUnderlyingObject(First.ResultPtr);
    Value *SecondUnderlying = getUnderlyingObject(Second.ResultPtr);
    bool MayWrite = !CI->getCalledFunction() || // 间接调用：保守假定可写
                    (!CI->doesNotAccessMemory() && !CI->onlyReadsMemory());

    if (MayWrite) {
      for (auto &Arg : CI->args()) {
        Value *A = Arg.get();
        // 跳过 LoadInst：调用接收的是加载后的值，不是存储位置地址
        if (isa<LoadInst>(A))
          continue;
        Value *AUnderlying = getUnderlyingObject(A);
        if (AUnderlying == FirstUnderlying || AUnderlying == SecondUnderlying) {
          LLVM_DEBUG(dbgs()
                     << "    call writes to result ptr: " << *CI << "\n");
          return true; // 调用可能覆盖 QI 结果
        }
      }
    }

    //=== Release 检查：vtable[2] dispatch 在同一对象上 ===
    if (!CI->getCalledFunction()) {
      Value *Callee = CI->getCalledOperand();
      if (auto *L = dyn_cast<LoadInst>(Callee)) {
        Value *VPtr = L->getPointerOperand();

        if (auto *G = dyn_cast<GetElementPtrInst>(VPtr)) {
          if (G->getNumIndices() == 1) {
            if (auto *Idx = dyn_cast<ConstantInt>(G->getOperand(1))) {
              uint64_t IdxVal = Idx->getZExtValue();
              // vtable[2] = Release.
              // MSVC ABI: GEP i8, i64 16 (2 × sizeof(ptr))
              // Itanium:  GEP ptr, i64 2
              bool IsReleaseSlot = false;
              if (G->getSourceElementType()->isIntegerTy(8))
                IsReleaseSlot = (IdxVal == 16); // MSVC: 字节偏移
              else
                IsReleaseSlot = (IdxVal == 2); // Itanium: 元素索引
              if (IsReleaseSlot) {
                Value *VTablePtr = G->getPointerOperand();
                if (auto *VL = dyn_cast<LoadInst>(VTablePtr)) {
                  // Release 的 vtable 来源 == 第一个 QI 的对象？
                  if (getUnderlyingObject(VL->getPointerOperand()) ==
                      getUnderlyingObject(First.ObjectPtr))
                    return true; // 同一对象的 Release，阻止合并
                }
              }
            }
          }
        }
        // vtable[0] 的直接加载（无 GEP）→ 是 QueryInterface，不是 Release。
        // vtable[1] 也可能以类似方式访问，但那是 AddRef，不干扰合并。
      }
      // 通过无关对象进行的间接调用 → 安全
      return false;
    }

    // 直接调用（被调用者是已知函数）分析：
    // 它们操作的是接口指针指向的对象，而非被查询的对象本身。
    // 一个直接调用不可能是 Release，因为 Release 总是通过 vtable 间接调用。
    // 保守策略：允许所有直接调用。
    return false;
  }

  // Store 指令分析：
  //
  // 危险场景1：存储到第一个 QI 的结果存储位置 → QI 结果被覆盖
  //   例如：第一个 QI 将接口指针写入 %result1 (或 GEP %m_ptr)，
  //   然后某条 store 向同一位置写入新值。使用 getUnderlyingObject
  //   统一处理 alloca 和 GEP（com_ptr::m_ptr）场景。
  //
  // 危险场景2：存储到第一个 QI 的 ObjectPtr alloca → 对象身份改变
  //   例如：%this_alloca 先指向对象A（第一个 QI），后来被 store 为对象B。
  //   第二个 QI 查询对象B，但 Key 匹配的是对象A → 错误合并。
  if (auto *SI = dyn_cast<StoreInst>(I)) {
    Value *StoredPtr = SI->getPointerOperand();
    // 结果存储位置被覆盖（alloca / GEP 均适用）
    if (getUnderlyingObject(StoredPtr) == getUnderlyingObject(First.ResultPtr))
      return true;
    // 对象指针被替换
    if (StoredPtr == First.ObjectPtr ||
        getUnderlyingObject(StoredPtr) == getUnderlyingObject(First.ObjectPtr))
      return true;
    // 其他 store 不影响合并安全性
    return false;
  }

  // Load 指令分析：
  // 若某条指令从 Second.ResultPtr 加载数据，而我们要删除 Second，
  // 这会导致该 load 读取未初始化的内存。但在我们的合并策略中，
  // 我们会在删除 Second 之前将第一个 QI 的结果存入 Second 的 alloca，
  // 所以 load 读取的结果仍然是正确的。这里标记为干扰是保守处理。
  if (auto *LI = dyn_cast<LoadInst>(I)) {
    if (LI->getPointerOperand() == Second.ResultPtr)
      return true;
  }

  return false;
}

/// 判断第二个 QI 调用是否可以安全地复用第一个 QI 调用的结果。
///
/// 本函数实现三层安全检查：
///
/// 第1层：支配关系。First 必须支配 Second（First 在每条到达 Second 的
///        执行路径上都先执行）。这是合并的基本前提。
///
/// 第2层：参数相等性。两个 QI 的 ObjectPtr 和 GUIDPtr 穿透到底层后必须
///        指向同一来源。这保证了它们查询的是同一对象的同一接口。
///
/// 第3层：指令干扰检查。遍历 First 和 Second 之间的所有指令（按程序顺序），
///        检查是否有 Release、对象替换、结果覆盖等干扰。
///
/// 跨基本块场景的处理：从 Second 的 BB 沿立即支配者 (IDom) 链上溯到 First
/// 的 BB，检查路径上所有基本块的全部指令。这是保守但正确的方法 — 我们
/// 检查所有"可能执行"的路径上的指令，即使某些分支实际不会被走。
///
/// \param First 第一个（先执行的）QI 调用
/// \param Second 第二个（后执行的）QI 调用
/// \param DT 支配树，用于支配检查和 IDom 链遍历
/// \returns true 表示安全可合并
static bool safeToMerge(const QICall &First, const QICall &Second,
                        DominatorTree &DT) {
  // 第1层：支配关系 — First 必须在所有路径上都先于 Second 执行
  if (!DT.dominates(First.Call, Second.Call)) {
    LLVM_DEBUG(dbgs() << "    first doesn't dominate second (BB1="
                      << First.Call->getParent()->getName() << ", BB2="
                      << Second.Call->getParent()->getName() << ")\n");
    return false;
  }

  // 第2层：参数相等性 — 同一对象 + 同一 IID
  if (getUnderlyingObject(First.ObjectPtr) !=
      getUnderlyingObject(Second.ObjectPtr)) {
    LLVM_DEBUG(dbgs() << "    different object\n");
    return false;
  }
  if (getUnderlyingObject(First.GUIDPtr) !=
      getUnderlyingObject(Second.GUIDPtr)) {
    LLVM_DEBUG(dbgs() << "    different GUID\n");
    return false;
  }

  // 第3层：指令干扰检查
  BasicBlock *FirstBB = First.Call->getParent();
  BasicBlock *SecondBB = Second.Call->getParent();

  // 情形1：同一基本块 — 只需检查 First 和 Second 之间的指令
  if (FirstBB == SecondBB) {
    bool FoundFirst = false;
    for (Instruction &I : *FirstBB) {
      if (&I == First.Call) {
        FoundFirst = true;
        continue;
      }
      if (!FoundFirst)
        continue;
      if (&I == Second.Call)
        break; // 到达 Second，停止检查
      if (instructionInterferes(&I, First, Second)) {
        LLVM_DEBUG(dbgs() << "    interferes: " << I << "\n");
        return false;
      }
    }
    return true;
  }

  // 情形2：不同基本块 — 使用逆向 CFG 遍历
  //
  // IDom 链遍历不足以覆盖菱形 CFG 中的所有路径：
  //     FirstBB → A → SecondBB
  //             → B ↗
  // IDom(SecondBB)=FirstBB，A 和 B 不在链上。
  // 若 A 或 B 中有指令覆盖 First 的结果指针，IDom 链检查会漏掉。
  //
  // 正确方法：从 SecondBB 逆向 BFS 遍历 CFG，访问所有被 FirstBB 支配的
  // 前驱块。这覆盖了从 First 到 Second 的所有可能执行路径上的每条指令。
  {
    SmallPtrSet<BasicBlock *, 16> Visited;
    SmallVector<BasicBlock *, 16> WorkList;
    SmallVector<BasicBlock *, 16> RegionBBs; // First 和 Second 之间的所有 BB
    Visited.insert(SecondBB);
    WorkList.push_back(SecondBB);

    while (!WorkList.empty()) {
      BasicBlock *BB = WorkList.pop_back_val();
      for (auto *Pred : predecessors(BB)) {
        if (!Visited.insert(Pred).second)
          continue;
        // 只访问被 FirstBB 支配的块 — 这些块保证在 First 之后执行
        if (!DT.dominates(FirstBB, Pred))
          continue;
        if (Pred == FirstBB)
          continue; // FirstBB 单独处理
        RegionBBs.push_back(Pred);
        WorkList.push_back(Pred);
      }
    }

    // 检查 FirstBB 中 First.Call 之后的所有指令
    bool PastFirst = false;
    for (Instruction &I : *FirstBB) {
      if (&I == First.Call) {
        PastFirst = true;
        continue;
      }
      if (!PastFirst)
        continue;
      if (instructionInterferes(&I, First, Second)) {
        LLVM_DEBUG(dbgs() << "    cross-BB interferes in " << FirstBB->getName()
                          << ": " << I << "\n");
        return false;
      }
    }

    // 检查所有中间基本块的全部指令
    for (BasicBlock *BB : RegionBBs) {
      for (Instruction &I : *BB) {
        if (instructionInterferes(&I, First, Second)) {
          LLVM_DEBUG(dbgs() << "    cross-BB interferes in " << BB->getName()
                            << ": " << I << "\n");
          return false;
        }
      }
    }

    // 检查 SecondBB 中 Second.Call 之前的指令
    for (Instruction &I : *SecondBB) {
      if (&I == Second.Call)
        break;
      if (instructionInterferes(&I, First, Second)) {
        LLVM_DEBUG(dbgs() << "    cross-BB interferes in "
                          << SecondBB->getName() << ": " << I << "\n");
        return false;
      }
    }
  }
  return true;
}

/// 在接口指针上插入 vtable[1] AddRef 调用，补偿被消除的 QI 的隐式 AddRef。
///
///   合并前: QI1(+1)→Rel(-1)→QI2(+1)→Rel(-1)  = 净0
///   合并后: QI1(+1)→AddRef(+1)→Rel(-1)→Rel(-1) = 净0 ✓
///
/// \param ResultPtr 第一个 QI 的输出指针（QI 将接口指针写入 *ResultPtr）。
///                  插入点处从此地址加载接口指针即为 QI 返回的接口指针。
/// \param InsertBefore 新指令将插入在此指令之前
///
/// 生成的 IR 序列：
///   %qi.addref.ptr    = load ptr, ptr %ResultPtr
///   %qi.addref.vtable = load ptr, ptr %qi.addref.ptr
///   %qi.addref.slot   = getelementptr i8, ptr %qi.addref.vtable, i64 8
///   %qi.addref.fn     = load ptr, ptr %qi.addref.slot
///   %qi.addref        = call i32 %qi.addref.fn(ptr %qi.addref.ptr)
static void insertAddRefCall(Value *ResultPtr, Instruction *InsertBefore) {
  LLVMContext &Ctx = InsertBefore->getContext();
  Type *PtrTy = PointerType::getUnqual(Ctx);
  auto InsertPt = InsertBefore->getIterator();

  auto *IfacePtr = new LoadInst(PtrTy, ResultPtr, "qi.addref.ptr", InsertPt);

  auto *VTable = new LoadInst(PtrTy, IfacePtr, "qi.addref.vtable", InsertPt);

  // vtable[1] = AddRef slot. 使用 i8 作为 GEP 源元素类型，
  // 配合显式字节偏移 (pointer_size * 1)。
  // MSVC ABI: i8, i64 8; Itanium ABI: i8, i64 8 (均为指针大小)
  unsigned PtrSize =
      InsertBefore->getModule()->getDataLayout().getPointerSize();
  auto *AddRefSlot = GetElementPtrInst::Create(
      Type::getInt8Ty(Ctx), VTable,
      {ConstantInt::get(Type::getInt64Ty(Ctx), PtrSize * 1)}, "qi.addref.slot",
      InsertPt);

  auto *AddRefFn = new LoadInst(PtrTy, AddRefSlot, "qi.addref.fn", InsertPt);

  auto *AddRefFTy = FunctionType::get(Type::getInt32Ty(Ctx), {PtrTy}, false);
  CallInst::Create(AddRefFTy, AddRefFn, {IfacePtr}, "qi.addref", InsertPt);
}

/// 执行合并：用 AddRef 平衡引用计数，消除冗余 QI。
///
/// 返回值处理：
/// QueryInterface 返回 HRESULT (i32)。控制流能到达 Second QI 的前提
/// 是 First QI 成功（返回 S_OK/0），否则 C++/WinRT 会抛异常或提前返回。
/// 因此 Second QI 若执行也必定返回 0。消除后用常量 0 替代 Second QI
/// 的所有使用——这比 replaceAllUsesWith(First) 更语义精确（被消除的
/// QI 从未执行，返回值是虚构的），且能让后续 passes 将 HRESULT==0
/// 的检查分支折叠为常数。
static bool performMerge(const QICall &First, QICall &Second) {
  LLVM_DEBUG(dbgs() << "COMQIMerge: Merging QI call at " << *Second.Call
                    << " into " << *First.Call << "\n");

  LLVMContext &Ctx = Second.Call->getContext();
  Type *PtrTy = PointerType::getUnqual(Ctx);

  // 安全验证：ResultPtr 必须是合法的加载/存储目标。
  // 可能的有效类型：AllocaInst、GetElementPtrInst、BitCastInst。
  // 无效类型：PHINode、SelectInst、ConstantExpr 等不是存储地址的值。
  auto isValidStorage = [](Value *V) -> bool {
    while (auto *BC = dyn_cast<BitCastInst>(V))
      V = BC->getOperand(0);
    return isa<AllocaInst>(V) || isa<GetElementPtrInst>(V);
  };

  bool FirstValid = isValidStorage(First.ResultPtr);
  bool SecondValid = isValidStorage(Second.ResultPtr);
  bool SameResult = (First.ResultPtr == Second.ResultPtr);

  // 安全闸门：First.ResultPtr 无效 → 无法加载接口指针做 AddRef → 放弃
  if (!FirstValid) {
    LLVM_DEBUG(dbgs() << "  Skipping: First.ResultPtr is not valid storage\n");
    return false;
  }
  // Second.ResultPtr 无效且与 First 不同 → 无法复制结果 → 放弃
  if (!SecondValid && !SameResult) {
    LLVM_DEBUG(dbgs() << "  Skipping: Second.ResultPtr is not valid storage\n");
    return false;
  }

  // 在 First QI 之后立即插入补偿指令。
  // 关键：必须在 lifetime.end(First.ResultPtr) 之前完成所有对 First.ResultPtr
  // 的访问。若将 load 推迟到 Second.Call 位置，ResultPtr 的 alloca 可能已
  // 被 llvm.lifetime.end 标记为死，后端可能将其栈槽复用给其他变量。
  {
    Instruction *InsertBefore = First.Call->getNextNode();
    if (!InsertBefore)
      InsertBefore = First.Call->getParent()->getTerminator();

    // AddRef：补偿被消除 QI 的隐式 AddRef
    insertAddRefCall(First.ResultPtr, InsertBefore);

    // 若两个 QI 写入不同位置，在此处加载 First 的结果并存储到 Second 位置。
    // load 必须在 First.Call 的 BB 内执行（在 lifetime.end 之前），
    // store 必须在 Second.Call 之前执行。
    if (!SameResult) {
      // 提前加载：在 First 的 BB 中，AddRef 指令之后
      auto *LoadFromFirst =
          new LoadInst(PtrTy, First.ResultPtr, "qi.reuse", InsertBefore);
      // 延迟存储：在 Second.Call 的位置（Second 的 BB 中）
      auto StorePt = Second.Call->getIterator();
      new StoreInst(LoadFromFirst, Second.ResultPtr, StorePt);
    }
  }

  // 将被消除的 QI 调用替换为常量 0 (S_OK)
  auto *ZeroConst = ConstantInt::get(Type::getInt32Ty(Ctx), 0);
  Second.Call->replaceAllUsesWith(ZeroConst);
  Second.Call->eraseFromParent();
  ++NumQIMerged;
  return true;
}

/// Pass 主入口：对函数执行 COM QueryInterface 合并。
///
/// 算法概述
/// ========
///
/// 本 Pass 使用支配树前序遍历，对每个基本块 (BB)：
///   1. 扫描 BB 中的所有 CallInst，识别 QueryInterface 调用。
///   2. 对每个识别到的 QI 调用，在 AvailableQIs 映射中查找 (ObjectPtr, GUIDPtr)
///      相同的第一个 QI 调用。
///   3. 若找到且 safeToMerge() 返回 true，执行合并（删除冗余 QI）。
///   4. 否则，将该 QI 调用记录到 AvailableQIs 中，供后续 BB 使用。
///
/// 循环安全
/// ========
///
/// 支配树前序遍历保证每个 BB 只被访问一次，因此跨迭代合并不会发生。
/// 但循环内同一 alloca 可能在不同迭代中保存不同对象，导致 Key 误匹配。
/// 因此：当进入循环头 BB 时，清除 AvailableQIs 中所有位于该循环体内的条目。
/// 循环外的条目保留（支持"循环前 QI → 循环后 QI"的合并）。
///
/// 分析保留
/// ========
///
/// 本 Pass 不修改控制流或循环结构（只删除调用指令），因此保留
/// DominatorTree 和 LoopInfo 分析结果。
///
/// \param F 待优化的函数
/// \param AM 函数分析管理器，提供 DominatorTree 和 LoopInfo
/// \returns 包含保留的分析集的 PreservedAnalyses
PreservedAnalyses COMQIMergePass::run(Function &F,
                                      FunctionAnalysisManager &AM) {
  auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
  auto &LI = AM.getResult<LoopAnalysis>(F);

  /// 可用 QI 调用的注册表。
  ///
  /// 键 = (底层ObjectPtr, 底层GUIDPtr) — 同一对象+同一IID → 可合并。
  /// 值 = 该键对应的一条已执行（或可执行）的 QI 调用。
  ///
  /// 使用 DenseMap：OIKey 是 std::pair<Value*,Value*>，LLVM 提供了开箱
  /// 即用的 DenseMapInfo 特化。
  DenseMap<QIKey, QICall> AvailableQIs;

  /// 标记本轮是否有任何修改。
  /// 若始终为 false，返回 PreservedAnalyses::all() 以通知 PassManager
  /// 保留所有分析结果。
  bool Changed = false;

  // 支配树前序遍历工作列表。
  // pop_back_val() 是栈操作，结合 children push 实现深度优先遍历。
  SmallVector<DomTreeNode *, 32> WorkList;
  WorkList.push_back(DT.getRootNode());

  while (!WorkList.empty()) {
    DomTreeNode *Node = WorkList.pop_back_val();
    BasicBlock *BB = Node->getBlock();

    // 将当前节点的所有子节点压入工作列表，
    // 子节点将在后续迭代中被处理。
    for (auto *Child : Node->children())
      WorkList.push_back(Child);

    // 循环安全处理：清除循环体内的条目。
    //
    // 进入循环头 BB 时，AvailableQIs 中可能已经有来自上一轮迭代
    // （或循环体内其他 BB）的条目。这些条目指向的 alloca 可能在
    // 新的迭代中被重新赋值，导致对象身份改变。
    //
    // 但我们不能简单地 clear() 全部，因为循环前的 QI 调用仍然有效：
    //   QI1 (循环前) → 循环 → QI2 (循环后)
    // 这个场景下 QI2 应该可以复用 QI1 的结果。
    if (LI.isLoopHeader(BB)) {
      Loop *L = LI.getLoopFor(BB);
      SmallVector<QIKey, 8> ToRemove;
      for (auto &KV : AvailableQIs) {
        // 仅删除位于当前循环体内的条目
        if (L->contains(KV.second.Call->getParent()))
          ToRemove.push_back(KV.first);
      }
      for (auto &K : ToRemove)
        AvailableQIs.erase(K);
    }

    // 第一遍：扫描本 BB 中的所有调用，识别 QI 调用。
    // 同时完成 QICall 的构建（仅解析一次，无重复开销）。
    SmallVector<QICall, 4> QICallsInBB;
    for (auto &I : *BB) {
      auto *CI = dyn_cast<CallInst>(&I);
      if (!CI)
        continue;
      QICall QI;
      if (recognizeQueryInterface(CI, QI))
        QICallsInBB.push_back(QI);
    }

    // 第二遍：对本 BB 的每个 QI 调用尝试合并。
    //
    // 遍历顺序与 BB 内指令顺序一致（QICallsInBB 按插入顺序存储）。
    // 这保证了同一 BB 内，先出现的 QI 先被注册到 AvailableQIs 中。
    for (QICall &QI : QICallsInBB) {
      LLVM_DEBUG(dbgs() << "COMQIMerge: Found QI call: " << *QI.Call << "\n");

      // 构造查找键：穿透到底层的 (ObjectPtr, GUIDPtr)
      QIKey Key(getUnderlyingObject(QI.ObjectPtr),
                getUnderlyingObject(QI.GUIDPtr));
      auto It = AvailableQIs.find(Key);

      if (It != AvailableQIs.end()) {
        // 发现同 Key 的 QI 调用 — 候选合并！
        QICall &First = It->second;
        LLVM_DEBUG(dbgs() << "  Key match! First QI at " << *First.Call
                          << "\n  Second QI at " << *QI.Call << "\n");
        if (safeToMerge(First, QI, DT)) {
          LLVM_DEBUG(dbgs() << "  Merging!\n");
          if (performMerge(First, QI)) {
            Changed = true;
            // continue：不将 Second 注册到 AvailableQIs 中，
            // 因为它已被删除（合并到 First）。后续 QI 调用应与 First
            // 合并，因为 First 更早且支配性更强。
            continue;
          }
          // performMerge 因安全闸门拒绝 → 回退到正常注册路径
          LLVM_DEBUG(dbgs() << "  Merge rejected by safety gate.\n");
        }
        LLVM_DEBUG(dbgs() << "  Not safe to merge.\n");
      }

      // 将此 QI 调用注册为可用，供后续 BB 中的 QI 调用查找。
      // 注意：如果在上面找到了 Key 但不安全（例如中间有 Release），
      // 此处会将 First 覆盖为当前 QI。这是因为中间有干扰意味着
      // First 的结果已失效，后续应以当前 QI 的结果为准。
      AvailableQIs[Key] = QI;
    }
  }

  if (!Changed)
    return PreservedAnalyses::all();

  // 保留分析结果：我们没有修改控制流或循环结构。
  PreservedAnalyses PA;
  PA.preserve<DominatorTreeAnalysis>();
  PA.preserve<LoopAnalysis>();
  return PA;
}
