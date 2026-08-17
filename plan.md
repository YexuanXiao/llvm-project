# 协程协作取消（Collaborative Cancellation for Coroutines）—— LLVM 实现计划

> 对应提案：`proposal_asserts/proposal.bs`（PxxxxR0）
> 目标仓库：llvm-project（llvm / clang / libcxx）
> 状态：本文件是任务 2 的产出，任务 3 按此计划实施，实施中会更新每步的状态。
> **2026-08-17 设计修订（最终版）**：按评审意见，**零新增 LLVM intrinsic**。
> ① 取消标志的读写改为 **clang builtin**（前端直接降低为既有 `llvm.coro.promise` +
> 指针运算）；② **LLVM 不关心 handler 是否存在**——`CoroFrame` 为**所有有 promise
> 的协程**在 promise 之后无条件预留 1 字节取消标志（写入永不越界，宽松实现）；
> ③ `unhandled_cancellation` **可选**：promise 未声明时不生成任何取消代码（零诊断、
> 零检查、零代码）。`request_cancel()` / `cancel_requested()` 是**受约束成员**
> （requires 表达式要求 `declval<Promise&>().unhandled_cancellation()` 合法）：
> 对非取消感知协程，这两个成员**不存在**，编译期即拒绝调用。

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

- **所有**有 promise 的协程（即所有 C++ 协程）都携带该字段：LLVM 不关心 handler
  是否存在，无条件预留（绕过约束的写入不越界——宽松实现；语言层面
  `request_cancel()` 仅对取消感知协程存在，requires 编译期约束）；
- header 部分（resume/destroy 指针、promise 投影偏移）**完全不变** → `coro.promise`
  与 `coro.done` 的 ABI 稳定；
- 帧大小变化：i8 字段紧贴 promise 后，帧尾通常因对齐填充而无需增大（`FrameSize`
  向上取整到 `FrameAlign`）；即使增大也仅为 1 字节；
- 取消检查只发生在声明了 `unhandled_cancellation` 的协程的挂起点恢复块（前端生成）。

### 取消标志的定位方式（无新增 LLVM intrinsic）

标志地址 = `llvm.coro.promise(frame, alignof(Promise), false) + sizeof(Promise)`。

- libc++ 的 `request_cancel()` / `cancel_requested()` 是 `coroutine_handle<Promise>`
  的**模板成员**，编译期已知 `alignof(Promise)` / `sizeof(Promise)`，调用
  `__builtin_coro_request_cancel(handle, align, size)` /
  `__builtin_coro_cancel_requested(handle, align, size)`；
- clang 将这两个 builtin 直接降低为**既有** `llvm.coro.promise` intrinsic +
  GEP + `store`/`load`（见 `CGCoroutine.cpp` 的 `EmitCoroutineCancel`）——
  **零新增 LLVM intrinsic**；
- 挂起点恢复块中的取消检查（前端生成）使用同一公式，与 builtin 降低一致
  （帧布局保证 flag 是 promise 之后第一个字节）。

### LLVM 侧改动（零新增 intrinsic，仅 1 处帧布局）

`CoroFrame::buildFrameLayout` 在 promise 字段之后添加 i8 header 字段
（**所有有 promise 的 switch ABI 协程**——即所有 C++ 协程，无条件，不检查
handler 是否存在）：

```cpp
// 有 promise 的协程无条件（LLVM 不关心 handler）：
if (PromiseAlloca)
  B.addField(Type::getInt8Ty(F.getContext()), MaybeAlign(), /*header*/ true);
```

无 promise 的协程（纯手写 IR）不保留标志——前端 builtin 需要
`sizeof(promise)` 编译期常量，无法作用于它们；这样既有手工协程测试的帧
布局逐字节不变。LLVM 不再有任何 marker、扫描、清理逻辑；
`CoroShape`/`Coroutines.cpp`/`CoroSplit.cpp` 均无取消相关代码。控制流检查
全部在前端。

### 前端生成的 IR 结构（取消感知协程）

```llvm
entry:                      ; ramp：promise 构造之后、初始挂起之前
  store i8 0, ptr <promise + sizeof(promise)>   ; 标志初始化（一次）
  ...
resume.0:                   ; 每个非 final 挂起点恢复块（await_resume 之前）
  %flag = load i8, ptr <promise + sizeof(promise)>   ; 前端生成
  br i1 %flag, label %coro.cancel, label %resume.0.cont
resume.0.cont:
  ... await_resume ...
  ...
coro.cancel:                ; 取消路径（所有恢复点检查跳转而来）
  call void @llvm.coro.cancelled()        ; 标记块
  call void @promise.unhandled_cancellation()
  br label %coro.final
coro.final:                 ; co_await promise.final_suspend() 的既有代码
  ...
```

前端在每个**挂起点恢复块**（`*.ready` 块，await_resume 之前）生成取消检查，
检查失败（标志非零）则跳转到 `coro.cancel` 块：

```llvm
resume.0:                   ; 第 0 个挂起点的恢复块
  %p = call ptr @llvm.coro.promise(ptr %frame, i32 alignof(Promise), i1 false)
  %flag = load i8, ptr %p + sizeof(Promise)     ; 取消标志
  br i1 %flag, label %coro.cancel, label %resume.0.cont
resume.0.cont:
  ... await_resume ...
```

```llvm
coro.cancel:                ; 取消路径（所有恢复点检查跳转而来）
  call void @llvm.coro.cancelled()        ; 标记块（供 CoroFrame 检测帧布局）
  call void @promise.unhandled_cancellation()
  br label %coro.final
coro.final:                 ; co_await promise.final_suspend() 的既有代码
  ...
```

标志初始化也由前端完成：promise 构造后、初始挂起前 `store 0`（一次）。

LLVM 侧职责仅剩数据布局：`CoroFrame` 为**所有**协程在 promise 后无条件预留
1 字节（i8 header 字段）；控制流检查全部在前端，LLVM 不关心 handler 是否存在。

取消路径在 resume 克隆中的执行流：
`恢复点检查 → coro.cancel（unhandled_cancellation()）→ coro.final → coro.save
（markCoroutineAsDone，将 resume 指针置 null）→ coro.suspend（resume 克隆中被替换
为 0）→ fs.resume → coro.ret`。若 `unhandled_cancellation()` 抛出异常，则沿
EH cleanup 路径执行 `coro.end(unwind)` + 帧释放后异常传播给 resumer——与
`unhandled_exception()` 抛异常的实现路径一致（协程被销毁，非"暂停在 final
suspend 点"；标准原文有 CWG2934 措辞缺陷，Clang 行为正确，勿按原文"暂停"
语义实现）。

destroy/cleanup 克隆中，恢复点块的唯一入口是恒假条件（destroy 从不
resume），`instcombine` 折叠后检查与取消块自动删除——无需任何 LLVM 侧
补丁清理。

---

## 1. 任务分解与实施步骤

### 步骤 A：LLVM IR 层（llvm/）

| # | 文件 | 改动 | 状态 |
|---|------|------|------|
| A1 | `llvm/lib/Transforms/Coroutines/CoroFrame.cpp` | `buildFrameLayout`：在 promise 字段后添加 i8 header 字段（**所有有 promise 的协程无条件**，即所有 C++ 协程；偏移 = promise 末尾，前端用 `coro.promise` + `sizeof(promise)` 定位；无 promise 的手写 IR 协程不添加，布局不变） | ✅ 已完成 |
| A2 | `llvm/test/Transforms/Coroutines/coro-cancel.ll` | 新测试：无 marker；取消感知协程的恢复点检查保留于 resume 克隆；destroy/cleanup 无取消路径（instcombine 折叠）；有 promise 但无 handler 的协程无检查但有标志字节 | ✅ 已完成 |

> 说明：`Intrinsics.td`、`CoroInstr.h`、`CoroShape.h`、`Coroutines.cpp`、
> `CoroSplit.cpp` **零改动**——LLVM 不关心取消，只提供 flag。

### 步骤 B：Clang 前端（clang/）

| # | 文件 | 改动 | 状态 |
|---|------|------|------|
| B1 | `clang/include/clang/Basic/Builtins.td` | 新增 `__builtin_coro_request_cancel(ptr, i32, i32)`、`__builtin_coro_cancel_requested(ptr, i32, i32) -> bool`（handle + alignof + sizeof） | ✅ 已完成 |
| B2 | `clang/include/clang/AST/StmtCXX.h` | `CoroutineBodyStmt`：`SubStmt` 枚举新增 `OnCancellation`；`CtorArgs` 与访问器新增 `OnCancellation` | ✅ 已完成 |
| B3 | `clang/lib/Sema/SemaCoroutine.cpp` | 新增 `CoroutineStmtBuilder::makeOnCancellation()`：查找 promise 的 `unhandled_cancellation`；未找到不诊断（取消可选）；找到则生成调用存到 `OnCancellation` | ✅ 已完成 |
| B4 | `clang/lib/CodeGen/CGCoroutine.cpp` | ① `EmitCoroutineBody`：promise 构造后初始化标志为 0（store i8 0）、创建 `coro.cancel` 块，最后填充（`unhandled_cancellation` 调用 + 跳转 final）；② `emitSuspendExpression`：每个非 final 挂起点的恢复块（`*.ready`，await_resume 之前）插入取消检查（`coro.promise` + GEP + load + 条件分支）；③ `EmitCoroutineCancel` 把两个 builtin 降低为既有 `coro.promise` + load/store；④ 无 handler 时上述一切不生成 | ✅ 已完成 |
| B5 | `clang/test/SemaCXX/coro-cancel.cpp` | 新测试：promise 含/不含 `unhandled_cancellation` 的协程可编译；builtin 用法 | ✅ 已完成 |
| B6 | `clang/test/CodeGenCoroutines/coro-cancel.cpp` | 新测试：生成的 IR 含 `coro.cancel` 块、`unhandled_cancellation` 调用、builtin 降低（无 marker） | ✅ 已完成 |

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

`buildFrameLayout` 中，在 promise 字段之后以 **header 字段**追加（所有有
promise 的 switch ABI 协程无条件——即所有 C++ 协程；保证偏移 = promise
末尾，与前端 builtin 计算一致）：

```cpp
// 有 promise 的协程无条件（LLVM 不检查 handler）：
if (PromiseAlloca)
  B.addField(Type::getInt8Ty(F.getContext()), MaybeAlign(), /*header*/ true);
```

由于 `B.addField`（header 模式）对 header 字段紧凑排列（`alignTo(StructSize,
FieldAlignment)`），取消标志偏移 = `alignTo(2*ptrsize + sizeof(Promise), 1)` =
promise 末尾，与 `__builtin_coro_cancel_requested` 的降低（`promise 指针 +
sizeof(Promise)`）一致；resume/destroy 函数指针与 promise 的偏移不受影响。
帧大小向上取整到 `FrameAlign`，通常不增加分配大小。

### 2.2 恢复点检查（clang 前端）

`emitSuspendExpression` 的 `EmitBlock(ReadyBlock)` 之后、await_resume 之前
插入检查；`CGCoroData` 新增 `CancelBB` / `PromiseAlign` / `PromiseSize` 字段
供所有挂起点共享。语义：取消在挂起时生效，恢复时在恢复点检查，跳过
await_resume，进入取消块 → final suspend。

检查代码（每个非 final 挂起点，真实 clang 生成）：

```llvm
resume.0:                               ; 挂起点 0 的恢复块
  %p = call ptr @llvm.coro.promise(ptr %frame, i32 8, i1 false)
  %flag.ptr = getelementptr i8, ptr %p, i64 8     ; + sizeof(Promise)
  %flag = load i8, ptr %flag.ptr, align 1
  br i1 %flag, label %coro.cancel, label %resume.0.cont
```

取消块可达性：由所有恢复点检查的跳转保证（每个挂起点都指向它），无需
CoroSplit 保活。destroy/cleanup 克隆中，恢复块仅经恒假条件可达
（destroy 从不 resume），`instcombine` 折叠后检查与取消块自动删除。

### 2.3 builtin 降低（clang 前端）

`__builtin_coro_request_cancel(handle, align, size)` /
`__builtin_coro_cancel_requested(handle, align, size)` 由
`EmitCoroutineCancel` 降低为既有 intrinsic：

```llvm
%p = call ptr @llvm.coro.promise(ptr %handle, i32 %align, i1 false)
%flag.ptr = getelementptr i8, ptr %p, i64 %size
store i8 1, ptr %flag.ptr            ; request_cancel
; 或
%flag = load i8, ptr %flag.ptr        ; cancel_requested
%r = icmp ne i8 %flag, 0
```

标志地址 = promise 末尾 = 帧布局保证的 i8 header 字段，与恢复点检查一致。

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

1. **帧头部不变**：resume/destroy 函数指针、promise 投影偏移（`coro.promise`）、
   `coro.done` 语义均不变。新增 i8 标志字段位于 promise 之后（header 区，
   `FrameTypeBuilder` 紧凑排列）；帧总大小向上取整到 `FrameAlign`，通常不增加。
   无 promise 的手写 IR 协程不添加字段，帧布局逐字节不变。
2. **控制流不变**：不取消感知的协程无任何检查代码；取消感知协程每次 resume
   多一次 load+branch（挂起点恢复块中，前端生成）。
3. **链接时混用**：`request_cancel()` / `cancel_requested()` 是**受约束成员**：
   仅当 promise 声明了 `unhandled_cancellation` 时存在（requires 编译期约束，
   对非取消感知协程无法调用）。实现是**宽松**的：标志字节对任何有 promise
   的协程无条件保留，绕过约束的写入不会越界——但这是实现细节，标准不承诺。

## 4. 风险与缓解

| 风险 | 缓解 |
|------|------|
| 取消标志使帧大小增加 | i8 紧贴 promise 后，帧大小向上取整到 `FrameAlign`，通常不增加；实测只有高对齐 promise 才可能多一个对齐槽（`coro-spill-promise*.ll`：align 64 promise 使帧 128→192），既有 `coro-frame-*` 回归已验证其余全部不变 |
| `unhandled_cancellation` 抛出异常时 destroy 函数行为 | 与 `unhandled_exception` 抛出时完全一致（协程被销毁，CWG2934 修正语义；勿按标准原文"暂停在 final suspend 点"实现），复用 `HasUnwindCoroEnd` 机制 |
| destroy/cleanup 克隆中恢复点检查残留（恒假条件可达） | `instcombine` 折叠后自动清除（`coro-cancel.ll` 的 RUN 行含 `instcombine` 验证）；真实 -O2 管线同样收敛 |
| 依赖 promise 类型（模板）时 `unhandled_cancellation` 查找 | 与 `unhandled_exception` 相同的依赖处理路径（依赖类型时不做成员查找，由实例化决定） |

## 5. 测试计划

- **llvm 层**：`coro-cancel.ll`（帧布局 CHECK、恢复点检查保留于 resume 克隆、destroy/cleanup 无取消路径）、既有 `coro-frame*`/`coro-split*` 回归。
- **clang 层**：`SemaCXX/coro-cancel.cpp`（语义）、`CodeGenCoroutines/coro-cancel.cpp`
  （IR 生成：标志初始化、恢复点检查、取消块、builtin 降低）。
- **libcxx 层**：`coroutine.handle/cancel.pass.cpp`（库 API + 与编译器配合的行为）。
- **ABI 回归**：编译现有 `CodeGenCoroutines` 全部用例，比较 IR 无取消相关差异。
- **端到端**：`/tmp/coro_test/multi_suspend_test.cpp`（多挂起点取消在第 2 个恢复点
  生效、跳过后续 body）与 6 项 `test_cancel.cpp` 断言。

## 5.1 验证结果（第 4 代，全部通过）

- 新增 3 测试全过；226 项全量回归：222 通过 + 4 Unsupported（无关），0 失败。
- 端到端：`test_cancel` 6 项 + `multi_suspend_test` + `cwg2934_test` 全 PASS。
- 性能：depth3 取消链 4.7µs vs stock 11.8µs（2.5×）；depth5 4.8µs vs 17.5µs
  （3.6×）；sink 一致（800000/2000000），无幽灵值。
- **因 +1 字节 flag 更新的既有测试**（帧布局 CHECK）：
  - `coro-spill-promise.ll` / `coro-spill-promise-02.ll`：align 64 promise，帧
    128→192（`malloc` 与 3 处 `dereferenceable`）；
  - `coro-frame-reuse-alloca-01/02/04/05.ll`、`coro-byval-param.ll`：1 字节
    promise，后续字段偏移 +1（如 index 17→18；align 32 的 b 槽位 544 不变——
    对齐取整恰好吸收）；
  - `coro-destructor-of-final_suspend.cpp`：index 移到偶数偏移后 load 对齐
    推断 1→2。

## 6. 后续（不在本次实施范围）

- `std::generator` 正式采用取消传播（见 `proposal_asserts/generator_improved.hpp`
  的实验实现与 `benchmark/` 对比结论）。
- CIR 前端（clang/lib/CIR）同步支持（当前 LLVM 中 CIR 未启用 coroutine 全流程）。
- MSVC ABI（`coro.id` 的 msvc 变体）支持（当前实现聚焦 switch-resume ABI）。
