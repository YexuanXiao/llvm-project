# 协程协作取消（Collaborative Cancellation for Coroutines）—— LLVM 实现计划

> 对应提案：`proposal_asserts/proposal.bs`（PxxxxR0）
> 目标仓库：llvm-project（llvm / clang / libcxx）
> 状态：本文件是任务 2 的产出，任务 3 按此计划实施，实施中会更新每步的状态。
> **2026-08-17 设计修订**：按评审意见，**非必要不增加 LLVM intrinsic**。
> 取消标志的读写改为 **clang builtin**（前端直接降低为既有 `llvm.coro.promise` +
> 指针运算），LLVM 层只保留 **1 个 marker intrinsic**（`llvm.coro.cancelled`，
> 用于定位取消入口块，无 ABI 影响）。

---

## 0. 总体设计摘要

提案要求两项新能力：

1. **库层**：`std::coroutine_handle::request_cancel()` / `cancel_requested()`。
2. **核心语言层**：取消感知（cancellation-aware）的协程在"已取消状态下被恢复"时，
   不继续执行挂起点之后的代码，而是执行 `promise.unhandled_cancellation()`，
   随后执行 `co_await promise.final_suspend()`。

### 不破坏现有 ABI 的扩展点（关键设计决策）

LLVM switch-resume ABI 的协程帧布局为：

```
offset 0           : resume 函数指针（固定）
offset ptrsize     : destroy 函数指针（固定）
（对齐后）          : promise 对象（固定，coro.promise 依赖此偏移）
（对齐后）          : suspend index 字段（非 header）
...                : spill 值 / alloca
```

**扩展点**：取消标志（i8，只需 1 bit）作为**新的 header 帧字段**加入，**紧跟
promise 对象之后**（由 `CoroFrame::buildFrameLayout` 统一布局；i8 自然对齐，
紧贴 promise 末尾，无填充）。

- 只有**取消感知**（promise 声明了 `unhandled_cancellation`）的协程才携带该字段；
- header 部分（resume/destroy 指针、promise 投影偏移）**完全不变** → `coro.promise`
  与 `coro.done` 的 ABI 稳定；
- 不使用新标志的既有协程，帧布局与现在**逐字节相同**；
- 恢复时的取消检查只发生在取消感知协程的 resume 函数入口（CoroSplit 生成）。

### 取消标志的定位方式（无新增 LLVM intrinsic）

标志地址 = `llvm.coro.promise(frame, alignof(Promise), false) + sizeof(Promise)`。

- libc++ 的 `request_cancel()` / `cancel_requested()` 是 `coroutine_handle<Promise>`
  的**模板成员**，编译期已知 `alignof(Promise)` / `sizeof(Promise)`，调用
  `__builtin_coro_request_cancel(handle, align, size)` /
  `__builtin_coro_cancel_requested(handle, align, size)`；
- clang 将这两个 builtin 直接降低为**既有** `llvm.coro.promise` intrinsic +
  GEP + `store`/`load`（见 `CGCoroutine.cpp` 的 `EmitCoroutineCancel`）——
  **零新增 LLVM intrinsic**；
- CoroSplit 的 resume 入口检查使用 `CancelFlagOffset`（帧布局时记录），
  与前端公式一致（帧布局保证 flag 是 promise 之后第一个字节）。

### IR 层接口（1 个新 marker intrinsic）

```tablegen
// Intrinsics.td 中新增
def int_coro_cancelled : Intrinsic<[], [], []>;
```

- `llvm.coro.cancelled()`：**无操作标记 intrinsic**（无属性，不会被 DCE），
  前端把它作为取消入口块 `coro.cancel` 的第一条指令；CoroSplit 扫描它来定位
  取消入口块，随后在克隆与 ramp 中删除。（作用类似 `llvm.coro.suspend` 之于挂起点。）

### 前端生成的 IR 结构（取消感知协程）

```llvm
cleanup.cont:               ; 初始 suspend 恢复后的必经路径（body 之前）
  %flag = load i1, ptr <promise + sizeof(promise)>   ; 前端生成
  br i1 %flag, label %coro.cancel, label %coro.body.cont
coro.body.cont:             ; body 开头
  ...
try.cont:                   ; body 正常完成路径（异常处理器的继续块）
  br label %coro.final      ; 显式终止，防止被 EmitBlock 改道
coro.cancel:                ; 取消路径（resume.entry 与 body 前检查跳转而来）
  call void @llvm.coro.cancelled()        ; 标记块（同时使块可达）
  call void @promise.unhandled_cancellation()
  br label %coro.final
coro.final:                 ; co_await promise.final_suspend() 的既有代码
  ...
```

前端在 body 之前生成的取消检查有两个作用：① 使 `coro.cancel` 块**可达**
（CoroSplitPass::run 在 `Shape::analyze` 之前调用 `removeUnreachableBlocks`，
无前驱的块会被提前删除）；② 运行时是廉价 no-op（resume 入口已检查过标志）。

CoroSplit 的 `createResumeEntryBlock` 在 resume 函数入口插入：

```llvm
resume.entry:
  %cancel = load i1, ptr <frame + CancelFlagOffset>   ; 仅取消感知协程
  br i1 %cancel, label %coro.cancel, label %resume.switch
resume.switch:
  %index = load iN, ptr <frame + IndexOffset>
  switch iN %index, ...
```

取消路径在 resume 克隆中的执行流：
`resume.entry → coro.cancel（unhandled_cancellation()）→ coro.final → coro.save
（markCoroutineAsDone，将 resume 指针置 null）→ coro.suspend（resume 克隆中被替换
为 0）→ fs.resume → coro.ret`。若 `unhandled_cancellation()` 抛出异常，则沿
EH cleanup 路径执行 `coro.end(unwind)` + 帧释放后异常传播给 resumer——与
`unhandled_exception()` 抛异常的实现路径一致（协程被销毁，非"暂停在 final
suspend 点"；标准原文有 CWG2934 措辞缺陷，Clang 行为正确，勿按原文"暂停"
语义实现）。

---

## 1. 任务分解与实施步骤

### 步骤 A：LLVM IR 层（llvm/）

| # | 文件 | 改动 | 状态 |
|---|------|------|------|
| A1 | `llvm/include/llvm/IR/Intrinsics.td` | 新增 1 个 marker `int_coro_cancelled : Intrinsic<[], [], []>`（无参数、无属性，防 DCE） | ✅ 已完成 |
| A2 | `llvm/include/llvm/Transforms/Coroutines/CoroInstr.h` | 新增 `CoroCancelledInst` 包装类（含 `classof`） | ✅ 已完成 |
| A3 | `llvm/include/llvm/Transforms/Coroutines/CoroShape.h` | `SwitchLoweringStorage` 新增 `unsigned CancelFlagOffset; bool HasCancel; BasicBlock *CancelEntryBlock;` | ✅ 已完成 |
| A4 | `llvm/lib/Transforms/Coroutines/Coroutines.cpp` | `Shape::analyze` 收集 marker 并设置 `HasCancel` / `CancelEntryBlock`；登记到 `NonOverloadedCoroIntrinsics` | ✅ 已完成 |
| A5 | `llvm/lib/Transforms/Coroutines/CoroFrame.cpp` | `buildFrameLayout`：若 `HasCancel`，在 promise 字段后加 i8 header 字段并记录 `CancelFlagOffset`（保证紧贴 promise 末尾） | ✅ 已完成 |
| A6 | `llvm/lib/Transforms/Coroutines/CoroSplit.cpp` | ① `createResumeEntryBlock`：resume 入口插入取消标志 load + 分支到 `CancelEntryBlock`；② ramp 中初始化标志为 false（`updateCoroFrame` 后）；③ `postSplitCleanup` 与 ramp 清理删除 marker | ✅ 已完成 |
| A7 | `llvm/lib/Transforms/Coroutines/Coroutines.cpp` | marker 登记在 intrinsic 扫描表中（防止被误删） | ✅ 已完成（与 A4 合并） |
| A8 | `llvm/test/Transforms/Coroutines/coro-cancel.ll` | 新测试：帧布局含取消标志、resume 入口分支、marker 删除、destroy/cleanup 无取消路径 | ✅ 已完成 |

### 步骤 B：Clang 前端（clang/）

| # | 文件 | 改动 | 状态 |
|---|------|------|------|
| B1 | `clang/include/clang/Basic/Builtins.td` | 新增 `__builtin_coro_request_cancel(ptr, i32, i32)`、`__builtin_coro_cancel_requested(ptr, i32, i32) -> bool`（handle + alignof + sizeof） | ✅ 已完成 |
| B2 | `clang/include/clang/AST/StmtCXX.h` | `CoroutineBodyStmt`：`SubStmt` 枚举新增 `OnCancellation`；`CtorArgs` 与访问器新增 `OnCancellation` | ✅ 已完成 |
| B3 | `clang/lib/Sema/SemaCoroutine.cpp` | 新增 `CoroutineStmtBuilder::makeOnCancellation()`：查找 promise 的 `unhandled_cancellation`；未找到不诊断（取消可选）；找到则生成调用存到 `OnCancellation` | ✅ 已完成 |
| B4 | `clang/lib/CodeGen/CGCoroutine.cpp` | ① `EmitCoroutineBody`：body 前生成取消检查（`coro.promise` + GEP + load + 条件分支，使 `coro.cancel` 块可达），`FinalBB` 前生成 `coro.cancel` 块（marker + 调用 + 跳转），并显式终止插入点防改道；② `EmitCoroutineCancel` 把两个 builtin 降低为既有 `coro.promise` + load/store | ✅ 已完成 |
| B5 | `clang/test/SemaCXX/coro-cancel.cpp` | 新测试：promise 含/不含 `unhandled_cancellation` 的协程可编译；builtin 用法 | ✅ 已完成 |
| B6 | `clang/test/CodeGenCoroutines/coro-cancel.cpp` | 新测试：生成的 IR 含 `coro.cancel` 块、`unhandled_cancellation` 调用、`llvm.coro.cancelled` marker、builtin 降低 | ✅ 已完成 |

### 步骤 C：libc++（libcxx/）

| # | 文件 | 改动 | 状态 |
|---|------|------|------|
| C1 | `libcxx/include/__coroutine/coroutine_handle.h` | `coroutine_handle<Promise>` 新增 `request_cancel()` / `cancel_requested()`（模板成员，调用 builtin 并传 `alignof/sizeof(_Promise)`），加 `_LIBCPP_ASSERT_VALID_EXTERNAL_API_CALL(__is_suspended(), ...)`。`coroutine_handle<void>` 不提供（无法定位标志，见提案设计考虑） | ✅ 已完成 |
| C2 | `libcxx/include/coroutine` | 新成员随头文件自动暴露（无需改动） | ✅ 已完成 |
| C3 | `libcxx/test/std/language.support/coroutines/coroutine.handle/cancel.pass.cpp` | 新测试：`request_cancel`/`cancel_requested` 基本行为；`unhandled_cancellation` 协程被取消后恢复的行为 | 待实施 |

### 步骤 D：文档与示例（可选）

| # | 文件 | 改动 | 状态 |
|---|------|------|------|
| D1 | `llvm/docs/Coroutines.md` | 记录 `llvm.coro.cancelled` 的语义与帧扩展点 | 待实施 |

### 步骤 E：验证

| # | 内容 | 状态 |
|---|------|------|
| E0 | 端到端功能验证：`/tmp/coro_test/test_cancel.cpp` 6 项测试（初始挂起取消、done、中途取消、cancel_requested、正常恢复、中途正常完成回归）全部通过 | ✅ 已完成 |
| E1 | `llvm/test/Transforms/Coroutines/coro-cancel.ll` 通过 | ✅ 已完成 |
| E2 | `clang/test/SemaCXX/coro-cancel.cpp`、`clang/test/CodeGenCoroutines/coro-cancel.cpp` 通过 | ✅ 已完成 |
| E3 | 既有协程测试全量回归（`clang/test/CodeGenCoroutines/` 79 项、`llvm/test/Transforms/Coroutines/` 171 项）无新增失败 —— 证明 ABI 兼容 | ✅ 已完成 |
| E4 | libcxx coroutine_handle 相关测试通过 | 待实施（libcxx 未配置进当前构建，以 /tmp/coro_test 端到端测试替代，见 E0） |
| E5 | **CWG2934 语义验证**：`/tmp/coro_test/cwg2934_test.cpp` —— `unhandled_cancellation()` 抛出异常时：① 异常传播给 resumer ✓；② 协程标记为 done（resume 指针置 null），非"暂停在 final suspend 点" ✓；③ body 不执行（取消立即生效）✓；④ `destroy()` 释放帧 ✓（与 `unhandled_exception` 逃逸路径完全一致）。注：测试需用 `noinline` sink 阻止 CoroElide 省略帧分配，否则 dealloc_count=0 是省略的合法结果而非泄漏 | ✅ 已完成 |

---

## 2. 关键实现细节

### 2.1 帧布局（CoroFrame.cpp）

`buildFrameLayout` 中，在 promise 字段之后以 **header 字段**追加（保证偏移 =
promise 末尾，与 libc++ builtin 计算一致）：

```cpp
// Shape.SwitchLowering.HasCancel（取消感知协程）时：
auto *I1Ty = Type::getInt1Ty(F.getContext());
Shape.SwitchLowering.CancelFlagOffset =
    B.addField(I1Ty, MaybeAlign(), /*header*/ true);
```

由于 `B.addField`（header 模式）对 header 字段紧凑排列（`alignTo(StructSize,
FieldAlignment)`），取消标志偏移 = `alignTo(2*ptrsize + sizeof(Promise), 1)` =
promise 末尾，与 `__builtin_coro_cancel_requested` 的降低（`promise 指针 +
sizeof(Promise)`）一致；resume/destroy 函数指针与 promise 的偏移不受影响。

### 2.2 resume 入口检查（CoroSplit.cpp）

`createResumeEntryBlock` 中，在创建 index switch **之前**插入：

```cpp
if (Shape.SwitchLowering.HasCancel) {
  auto *CancelFlagPtr = Builder.CreateInBoundsPtrAdd(
      FramePtr, ConstantInt::get(Type::getInt64Ty(C),
                                 Shape.SwitchLowering.CancelFlagOffset),
      "cancel.flag.addr");
  auto *CancelFlag = Builder.CreateLoad(Builder.getInt1Ty(), CancelFlagPtr, "cancel.flag");
  auto *CancelEntryBB = /* 通过 llvm.coro.cancel 标记块定位 */;
  Builder.CreateCondBr(CancelFlag, CancelEntryBB, SwitchBB);
}
```

取消入口块定位：前端生成的 `coro.cancel` 块以 `call void @llvm.coro.cancelled()`
开头，且**前端在初始 suspend 之后、body 之前生成取消检查分支**（load 标志 +
条件跳转），使 `coro.cancel` 块有真实前驱（不会被 `CoroSplitPass::run` 在
`Shape.analyze` 之前的 `removeUnreachableBlocks` 删除）。`Shape.analyze` 扫描
函数记录 marker 所在块为 `CancelEntryBlock`；`createResumeEntryBlock` 用
`Builder.CreateCondBr(CancelFlag, CancelEntryBlock, SwitchBB)` 生成分支，克隆
时经 `VMap` 自动映射。destroy/cleanup 克隆中该块不可达，由
`handleCancellation`（CoroCloner.h）移除其指向/内部代码后，经
`removeUnreachableBlocks` 清除。

### 2.3 intrinsic 降低

在 CoroSplit（或 CoroEarly）中，遍历函数内所有 `coro.cancel_request`：

```cpp
Builder.SetInsertPoint(Inst);
auto *FlagPtr = Builder.CreateInBoundsPtrAdd(FramePtr, CancelFlagOffset);
Builder.CreateStore(ConstantInt::getTrue(C), FlagPtr);
Inst->eraseFromParent();
```

`coro.cancel_requested` 类似，`load` 后 `replaceAllUsesWith`。

### 2.4 Sema 侧（SemaCoroutine.cpp）

`makeOnCancellation()` 与 `makeOnException()` 结构相同，但**更宽松**：

```cpp
bool CoroutineStmtBuilder::makeOnCancellation() {
  if (IsPromiseDependentType) return true;   // 依赖类型：运行时查找
  bool HasCancellation;
  LookupResult LR = lookupMember(S, "unhandled_cancellation", PromiseRecordDecl, Loc, HasCancellation);
  if (!HasCancellation) return true;          // 可选成员，不诊断
  ExprResult Cancellation = buildPromiseCall(S, Fn.CoroutinePromise, Loc, "unhandled_cancellation", {});
  Cancellation = S.ActOnFinishFullExpr(...);
  this->OnCancellation = Cancellation.get();
  return true;
}
```

注意：`unhandled_cancellation` 的调用**不需要** try-catch 包裹（与
`unhandled_exception` 不同）：取消处理本身抛出的异常直接传播给 resumer，
同时协程帧被销毁（CWG2934 修正后的语义，与 `unhandled_exception` 抛出时的
实际行为一致；提案措辞 [dcl.fct.def.coroutine] p18 扩展）。

### 2.5 CodeGen 侧（CGCoroutine.cpp）

在 `EmitCoroutineBody` 中调整：

```cpp
// 原有：EmitBlock(FinalBB); EmitStmt(S.getFinalSuspendStmt());
// 改为：
if (auto *CancelHandler = S.getCancellationHandler()) {
  auto *CancelBB = createBasicBlock("coro.cancel");
  EmitBlock(CancelBB);
  Builder.CreateCall(CGM.getIntrinsic(llvm::Intrinsic::coro_cancel), {});
  EmitStmt(CancelHandler);
  Builder.CreateBr(FinalBB);
}
EmitBlock(FinalBB);
EmitStmt(S.getFinalSuspendStmt());
```

`unhandled_cancellation` 调用需要 `coroutine_handle::from_promise(*this)`
语义（与 `unhandled_exception` 相同，通过既有 `buildPromiseCall` 的隐式
`this` 完成）。

### 2.6 libc++ 侧

```cpp
// coroutine_handle<void>
_LIBCPP_HIDE_FROM_ABI void request_cancel() const {
  _LIBCPP_ASSERT_VALID_EXTERNAL_API_CALL(__is_suspended(), "request_cancel() can be called only on suspended coroutines");
  __builtin_coro_cancel_request(__handle_);
}
[[nodiscard]] _LIBCPP_HIDE_FROM_ABI bool cancel_requested() const {
  _LIBCPP_ASSERT_VALID_EXTERNAL_API_CALL(__is_suspended(), "cancel_requested() can be called only on suspended coroutines");
  return __builtin_coro_cancel_requested(__handle_);
}
```

---

## 3. ABI 兼容性论证（不破坏现有 ABI）

1. **既有协程**：不使用任何新 intrinsic，`Shape.CancelInsts` 为空，
   `buildFrameLayout` 不添加字段 → 帧布局、`coro.promise` 偏移、`coro.done`
   语义逐字节不变。
2. **取消感知协程**：
   - header（resume/destroy 指针 + promise）不变 → `coro.promise` 投影兼容；
   - 新增字段位于非 header 区域，由 `FrameTypeBuilder` 自动布局；
   - resume 函数入口多一次 load+branch，仅在取消感知协程上发生。
3. **链接时混用**：`request_cancel()` 作用于取消感知协程才有定义好的行为；
   对非取消感知协程调用属于提案规定的 UB（前置条件违反），与
   `resume()` 对 final-suspend 协程调用是 UB 的情形一致。

## 4. 风险与缓解

| 风险 | 缓解 |
|------|------|
| 取消标志与 index 字段的对齐/布局在极端情况下与既有帧不同 | 通过 `llvm/test/Transforms/Coroutines/coro-frame-*.ll` 全量回归验证；`FrameTypeBuilder` 已有成熟的字段对齐逻辑 |
| `unhandled_cancellation` 抛出异常时 destroy 函数行为 | 与 `unhandled_exception` 抛出时完全一致（协程被销毁，CWG2934 修正语义；勿按标准原文"暂停在 final suspend 点"实现），复用 `HasUnwindCoroEnd` 机制 |
| resume 克隆中 `cancel.entry` 块不可达导致被删 | 通过 `llvm.coro.cancel` 标记块 + 克隆后 VMap 映射保证；测试覆盖 |
| 依赖 promise 类型（模板）时 `unhandled_cancellation` 查找 | 与 `unhandled_exception` 相同的依赖处理路径（依赖类型时不做成员查找，由实例化决定） |

## 5. 测试计划

- **llvm 层**：`coro-cancel.ll`（帧布局 CHECK、resume.entry 分支 CHECK、intrinsic
  降低 CHECK）、既有 `coro-frame*`/`coro-split*` 回归。
- **clang 层**：`SemaCXX/coro-cancel.cpp`（语义）、`CodeGenCoroutines/coro-cancel.cpp`
  （IR 生成，仿照 `coro-unhandled-exception.cpp` 的双 triple 模式）。
- **libcxx 层**：`coroutine.handle/cancel.pass.cpp`（库 API + 与编译器配合的行为）。
- **ABI 回归**：编译现有 `CodeGenCoroutines` 全部用例，比较 IR 无取消相关差异。

## 6. 后续（不在本次实施范围）

- `std::generator` 正式采用取消传播（见 `proposal_asserts/generator_improved.hpp`
  的实验实现与 `benchmark/` 对比结论）。
- CIR 前端（clang/lib/CIR）同步支持（当前 LLVM 中 CIR 未启用 coroutine 全流程）。
- MSVC ABI（`coro.id` 的 msvc 变体）支持（当前实现聚焦 switch-resume ABI）。
