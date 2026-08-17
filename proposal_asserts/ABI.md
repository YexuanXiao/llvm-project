# 协程协作取消的 ABI 规格

本文档定义"协作取消"（PxxxxR0）在 Itanium ABI 与 MSVC ABI 下的 ABI 约束，
保证：

1. **不启用取消的协程 ABI 完全不变**（帧布局逐字节相同）；
2. **启用取消的协程，`resume`/`destroy`/`done` 等既有操作的 ABI 不变**——
   一个不感知取消的库（例如导出了 `void resume(std::coroutine_handle<>)`）
   对取消感知协程的句柄调用仍然有效；
3. **`request_cancel` / `cancel_requested` 有明确定义的 ABI**，使 GCC、Clang、
   MSVC 可以互操作协程句柄。

目录：

1. [背景：协程帧的既有 ABI](#1-背景协程帧的既有-abi)
2. [ABI 兼容性原则](#2-abi-兼容性原则)
3. [取消标志的布局](#3-取消标志的布局)
4. [取消感知协程的帧布局](#4-取消感知协程的帧布局)
5. [`request_cancel` / `cancel_requested` 的 ABI](#5-request_cancel--cancel_requested-的-abi)
6. [互操作规则（GCC / Clang / MSVC）](#6-互操作规则gcc--clang--msvc)
7. [实现层映射（LLVM IR）](#7-实现层映射llvm-ir)
8. [未定义行为与前置条件](#8-未定义行为与前置条件)
9. [兼容性论证](#9-兼容性论证)

---

## 1. 背景：协程帧的既有 ABI

C++ 标准将协程状态（frame）的实现细节留给实现，但标准库的
`std::coroutine_handle<Promise>` 提供 `from_promise` / `promise` /
`address`，且 `coroutine_traits` 在模板层将 promise 类型与协程绑定。
在 LLVM 的 switched-resume ABI（Clang/GCC 在 Itanium 与 MSVC 目标上共用）中，
协程帧的头部布局为：

```
偏移 0             resume 函数指针（返回 void，参数为帧指针）
偏移 ptrsize      destroy 函数指针
偏移 2*ptrsize    对齐后的 promise 对象（size=sizeof(Promise), align=alignof(Promise)）
之后             suspend index、spilled values、allocas（由编译器自由布局）
```

其中 promise 的偏移由 `llvm.coro.promise` 内在计算：

```
promise_offset = alignTo(2 * ptrsize, alignof(Promise))
```

`llvm.coro.promise` 的降低与目标 ABI 无关（`CoroEarly.cpp` 中的
`lowerCoroPromise` 使用相同的"两个函数指针 + 对齐"模型计算偏移），因此
Itanium 与 MSVC 目标下该偏移公式一致。

`std::coroutine_handle<Promise>` 的互操作 ABI 由既有标准定义：
`from_address`/`address`（帧指针）、`from_promise`/`promise`（经
`llvm.coro.promise` 投影）、`resume`/`destroy`/`done`（经帧头函数指针与
resume 指针置空）。本提案**不修改**上述任何一条。

## 2. ABI 兼容性原则

| 场景 | 要求 |
|---|---|
| promise 未定义 `unhandled_cancellation`（非取消感知） | 帧布局、`resume`/`destroy`/`done`/`promise` 与提案前**逐字节相同**；无新增字段、无运行时开销 |
| promise 定义了 `unhandled_cancellation`（取消感知） | 帧头（resume/destroy 指针、promise 投影偏移）**不变**；新增 1 字节取消标志（见 §3）；`resume`/`destroy`/`done` 的**函数签名与调用约定不变** |
| 混合链接：非取消感知库操作取消感知句柄 | 库调用 `resume(h)`/`destroy(h)`/`done(h)`/`promise(h)` 走既有帧头，行为正确（见 §9） |
| 跨编译器：GCC/Clang/MSVC 共享句柄 | 句柄 = 帧指针（`void*`），标志位置由 §5 的规范公式确定，可互操作 |

## 3. 取消标志的布局

取消标志是取消感知协程帧中的一个 **1 字节（i8）字段**，位于 **promise
对象之后的第一字节**：

```
flag_address = llvm.coro.promise(frame, alignof(Promise), false)
               + sizeof(Promise)
```

- 该位置由帧布局保证：`CoroFrame::buildFrameLayout` 在 promise 字段之后以
  header 字段（自然对齐 1）追加 i8，紧贴 promise 末尾、无填充。
- 标志值：`0` = 未取消；`1` = 已请求取消。读写为**单字节、非原子**操作；
  并发 `request_cancel` 与 `resume`/`destroy` 构成数据竞争（与既有
  "并发恢复是 UB" 一致，见 [dcl.fct.def.coroutine]）。
- 标志生命周期：ramp 函数在 promise 构造后、初始挂起前将其初始化为 0；
  此后仅 `request_cancel` 置 1，`cancel_requested` 读取。协程销毁（destroy）
  时标志随帧释放，无需额外清理。

## 4. 取消感知协程的帧布局

```
偏移 0             resume 函数指针        （不变）
偏移 ptrsize      destroy 函数指针       （不变）
偏移 2*ptrsize    对齐后的 promise 对象   （不变，promise 投影 ABI 稳定）
偏移 promise 末尾  取消标志（i8）          （新增，仅取消感知协程）
之后             suspend index、spills 等（布局算法不变；取消感知协程的
                                      index 偏移可能因新增 1 字节而移动，
                                      但 index 偏移从不是跨编译器 ABI）
```

要点：

- **`resume`/`destroy` 函数指针的签名与位置不变**（帧头偏移 0 与 ptrsize）。
  取消感知协程的 `.resume`/`.destroy` 克隆函数与普通协程的签名完全一致：
  `void(ptr)`、相同调用约定、相同栈行为。
- **取消检查不在 resume 入口**：检查由前端生成在**每个挂起点恢复块**
  （`await_resume` 之前）。因此 `resume` 函数本身不含任何取消感知分支，
  "不感知取消的库对取消感知协程调用 `resume`"与对普通协程调用完全等价
  （见 §9）。
- 取消路径（`unhandled_cancellation()` + final suspend）是前端生成的普通
  CFG，不引入新的调用约定或特殊返回协议。

## 5. `request_cancel` / `cancel_requested` 的 ABI

### 5.1 语言层签名（标准库）

`std::coroutine_handle<Promise>` 的新成员（仅主模板，`coroutine_handle<void>`
不提供）：

```cpp
void request_cancel() const;   // 前置条件：*this 引用已暂停且未取消的协程
bool cancel_requested() const; // 前置条件：*this 引用已暂停的协程
```

句柄对象本身（含 `ptr` 成员）的 ABI 与既有 `coroutine_handle<Promise>` 完全
一致——**新成员不改变类布局**（成员函数不占用数据成员空间）。

`coroutine_handle<void>` 不提供新成员的原因：类型擦除的句柄不携带
`alignof/sizeof(Promise)`，无法按 §5.2 公式定位标志。需要操作类型擦除句柄
的代码应使用带 Promise 类型的句柄（`coroutine_handle<Promise>` 可隐式转换
为 `coroutine_handle<>`，反之不行），或在模板上下文中操作。

### 5.2 规范化操作语义（跨编译器契约）

为 GCC、Clang、MSVC 可互操作，两个操作的**规范定义**为：

```
request_cancel(promise, sizeP):
    flag = promise + sizeP                    // 1 字节
    *flag = 1

cancel_requested(promise, sizeP):
    flag = promise + sizeP                    // 1 字节
    return *flag != 0
```

其中：

- `promise` = `std::addressof(coroutine_handle<Promise>::promise())`
  （promise 对象地址，经既有 ABI 稳定投影获得，见下）；
- `sizeP` = `sizeof(Promise)`；

`promise` 的获得本身经既有投影：

```
promise = frame + alignTo(2 * ptrsize, alignP)   // alignP = alignof(Promise)
```

该投影是**既有 ABI**（`coroutine_handle<Promise>::promise()`，经
`llvm.coro.promise` / `__builtin_coro_promise`），本提案不新增。因此新
ABI 面**只有一个编译期常量 `sizeP`**；`alignP` 由既有 `promise()` 机制吸收，
不需要重新传递。

**关键点**：`sizeP` 是编译期已知常量，`promise` 地址由既有 ABI 投影得到，
**不依赖**编译器的帧布局细节（标志偏移由框架构保证为"promise 末尾"，见
§3）。任何编译器只要遵循此公式即可互操作。

### 5.3 编译内建接口（实现层）

clang 提供两个内建，作为 `request_cancel` / `cancel_requested` 的降低目标
（GCC/MSVC 可提供等价内建或按 §5.2 公式内联）：

```c
void __builtin_coro_request_cancel(void *promise, size_t sizeP);
bool __builtin_coro_cancel_requested(void *promise, size_t sizeP);
```

第一个参数是 **promise 对象地址**（调用方经 `&promise()` 获得），第二个是
`sizeof(Promise)`。两个内建均为 `nothrow`，只访问标志字节（`promise + sizeP`），
不访问帧内任何其他位置，对任意已暂停协程句柄调用不违反帧不变量。

## 6. 互操作规则（GCC / Clang / MSVC）

### 6.1 句柄传递

`std::coroutine_handle<Promise>` 的互操作规则沿用既有 C++ 协程 ABI：
句柄以不透明指针（帧指针）形式跨编译器传递；`from_promise`/`promise`/
`resume`/`destroy`/`done` 的行为由既有标准与各实现的 ABI 保证，本提案不
改变。取消感知协程的句柄与普通协程的句柄**无类型区分**——标志是帧内数据，
不是句柄的一部分。

### 6.2 取消操作互操作

- 编译器 A 创建取消感知协程（其 promise 定义了 `unhandled_cancellation`），
  将句柄传给编译器 B（GCC 或 MSVC 编译的库）。
- B 调用 `request_cancel(h)` / `cancel_requested(h)`：B 的库代码以
  `std::coroutine_handle<Promise>` 调用（Promise 类型在 B 侧可见），经其既有
  `promise()` 投影得到 promise 地址，按 §5.2 公式 `promise + sizeof(Promise)`
  计算标志地址。由于公式只依赖 ABI 稳定的 `sizeof(Promise)` 与既有投影，
  结果与 A 侧帧布局一致。
- A 侧随后 `resume(h)`：A 编译的 resume 克隆在挂起点恢复块检查标志（同一
  公式），执行取消路径。B 不需要了解 A 的任何实现细节。

### 6.3 非取消感知库与取消感知协程互操作

库导出的既有函数（如 `void run(std::coroutine_handle<task::promise_type>)`
内部调用 `resume(h)`）在取消感知协程上行为不变：`resume` 在挂起点恢复块
检查标志；若协程已被取消，则执行 `unhandled_cancellation` + final suspend
（与库内 `resume` 语义一致，库无需改动）。

### 6.4 MSVC 注意事项

- MSVC 的 `std::coroutine_handle<Promise>` 实现（MSVC STL）若未更新，则
  没有 `request_cancel`/`cancel_requested` 成员；互操作时可通过
  `__builtin_coro_*` 内建或手工内联 §5.2 公式实现（只需 `sizeof(Promise)`
  与既有 `promise()` 投影，均为标准 C++ 表达式）。
- MSVC 目标下协程帧同样走 LLVM switched-resume ABI（Clang 在 `-fms-extensions`
  / MSVC 目标下使用同一套 `llvm.coro.*` 内在与帧布局），因此 §3/§4/§5 的
  偏移公式在 MSVC 目标上不变。
- 既有警告：协程在 x86-32 的 MSVC ABI 上不稳定（`warn_coroutines_x86_windows`），
  取消标志作为帧内 i8 字段不改变该现状。

## 7. 实现层映射（LLVM IR）

### 7.1 标志读写

`request_cancel` / `cancel_requested` 降低为**既有** `llvm.coro.promise`
内在（在调用方 `promise()` 中）+ 指针运算（零新增操纵内在）：

```llvm
; promise() 的投影（既有 ABI）:
%p = call ptr @llvm.coro.promise(ptr %h, i32 alignP, i1 false)

; request_cancel(p, sizeP)
%flag = getelementptr i8, ptr %p, i64 sizeP
store i8 1, ptr %flag, align 1

; cancel_requested(p, sizeP)
%flag = getelementptr i8, ptr %p, i64 sizeP
%v = load i8, ptr %flag, align 1
%r = icmp ne i8 %v, 0
```

`llvm.coro.promise` 的降低（`lowerCoroPromise`）在 Itanium 与 MSVC 目标上
使用同一模型，投影偏移与 §5.2 一致。

### 7.2 取消检查（恢复点）

前端在**每个非 final 挂起点恢复块**、`await_resume` 之前生成：

```llvm
resume.N:
  %p = call ptr @llvm.coro.promise(ptr %frame, i32 alignP, i1 false)
  %flag = getelementptr i8, ptr %p, i64 sizeP
  %v = load i8, ptr %flag, align 1
  br i1 %v, label %coro.cancel, label %resume.N.cont
```

与 §7.1 使用同一公式，保证"跨编译器置标志"与"本地检查标志"一致。

### 7.3 新增 IR 项

- **无新增 intrinsic**。取消标志是帧中一个普通 i8 字段：`buildFrameLayout`
  为**所有有 promise 的 switch ABI 协程**（即所有 C++ 协程）在 promise 之后
  添加（`if (PromiseAlloca) B.addField(Type::getInt8Ty(...), MaybeAlign(),
  /*header*/ true)`），不依赖任何 marker 或形状分析。LLVM 不关心 handler
  是否存在——标志字节始终存在，即使违反库前置条件（对非取消感知协程调用
  `request_cancel()`，见 §8）写入也保持在帧内。无 promise 的手写 IR 协程
  不添加字段。
- 标志字段位于 promise 之后（§3），不进入任何既有的 ABI 投影
  （`coro.promise` 只投影 promise 本身）。

## 8. 未定义行为与前置条件

以下情形为 UB（与提案 [coroutine.handle.cancel] 的前置条件一致），
互操作实现不应依赖其行为：

| 情形 | 说明 |
|---|---|
| 对**非取消感知**协程调用 `request_cancel`/`cancel_requested` | 成员受 requires 约束：仅当 `declval<Promise&>().unhandled_cancellation()` 合法时存在（编译期检查），对非取消感知协程**无法调用**。绕过约束的调用（如非规范实现）是 UB；实现是宽松的：帧中标志字节对所有有 promise 的协程存在，写入不越界——但标准不承诺此行为 |
| 对**未暂停**协程调用 `request_cancel`/`cancel_requested` | 帧可能正在执行/已被销毁，数据竞争或悬垂访问 |
| 对已取消协程重复 `request_cancel` | 前置条件"尚未被取消"违反（第二次调用是 UB，尽管实现上只是重复置 1） |
| 并发 `request_cancel` 与 `resume`/`destroy` | 数据竞争（与并发 `resume` 的既有 UB 一致） |
| 对已取消协程调用 `resume` 之外的 `done`/`promise`/`destroy` | 行为与普通协程一致，**不**受取消状态影响（取消不改变这些操作的语义） |

注意：`request_cancel()` / `cancel_requested()` **只在 promise 声明了
`unhandled_cancellation` 时存在**（requires 约束，编译期强制）；未声明时这两个
成员不可用，无从调用。取消语义（执行 `unhandled_cancellation`）仅当 promise
声明了该成员才发生；未声明时编译器零诊断、零代码，对既有协程无任何影响。

最后一行是**有意设计**：取消状态只影响 `resume` 的后续执行（走取消
路径），不影响 `done`/`promise`/`destroy`。这保证了不感知取消的库操作
取消感知句柄时，除 `resume` 的语义（取消时执行 `unhandled_cancellation`）
外一切照旧。

## 9. 兼容性论证

### 9.1 非取消感知协程：ABI 无变化

- 有 promise 的协程帧中多一个 i8 标志字段（promise 之后），帧总大小向上
  取整到 `FrameAlign`，**通常不增加**；resume/destroy 函数指针与 promise
  投影偏移不变；
- 前端不生成恢复点检查（`CancelBB == nullptr` 时跳过）→ resume 克隆与
  提案前字节相同（除帧大小可能 +0~1 字节外）；
- 无 promise 的手写 IR 协程：帧布局逐字节不变；
- `coroutine_handle` 类布局不变（新增成员函数不占数据空间）。

### 9.2 取消感知协程：既有操作 ABI 无变化

- **`resume`/`destroy`**：函数指针仍在帧头偏移 0/ptrsize，签名
  `void(ptr)` 不变；resume 入口仍是 index switch（取消检查在恢复点，不在
  入口）。旧库的 `resume`/`destroy` 调用路径逐指令兼容。
- **`done`**：仍读帧头 resume 指针是否为 null，不受标志影响。
- **`promise`/`from_promise`**：投影偏移仍为 `alignTo(2*ptrsize, alignof(P))`，
  标志在 promise **之后**，不改变投影。
- **`address`/`from_address`**：句柄仍是帧指针，无包装、无标签。
- **调用约定**：取消路径是普通 CFG（分支 + 既有 final suspend），不引入
  新的调用约定、特殊返回寄存器或恢复协议。

### 9.3 导出库场景

```
库 libtask.a（未启用取消，导出）:
  void run(std::coroutine_handle<task::promise_type> h) { while (!h.done()) h.resume(); }

应用（启用取消，链接 libtask.a）:
  auto t = make_cancellable_task();   // 取消感知协程
  t.h.request_cancel();
  run(t.h);                            // 库内 resume 在恢复点检查标志，
                                       // 执行取消路径；库无需重编译
```

`run` 的调用：`resume(h)` 进入取消感知协程的 resume 克隆（签名兼容），
恢复点检查标志 → `unhandled_cancellation` → final suspend → `done()` 为真，
`run` 正常退出。库代码与数据均无需变化。

### 9.4 跨编译器互操作示例

```
GCC 编译的库 libx.a（导出）:
  void cancel(std::coroutine_handle<promise_type> h) { h.request_cancel(); }

Clang 编译的应用（链接 libx.a）:
  auto t = make_cancellable_task();   // Clang 生成的取消感知协程
  cancel(t.h);                        // GCC 的 request_cancel 按 §5.2 公式
                                      // 置标志；Clang 的恢复点检查读到标志
```

GCC 的 `request_cancel` 需要实现 §5.2 公式（GCC 若未实现提案，可用等价
内建/内联实现）；公式中的 `alignof/sizeof(Promise)` 与 `ptrsize` 均为 ABI
稳定值，不依赖编译器帧布局细节。

---

## 附：与草案措辞的对应

- [coroutine.handle.cancel]（新小节）：`request_cancel` / `cancel_requested`
  的签名、前置条件、效果——见 `proposal.bs` 的库部分。
- [dcl.fct.def.coroutine]：取消感知协程的替换体扩展（`unhandled_cancellation`
  + final suspend）、CWG2934 修正语义——见 `proposal.bs` 的核心语言部分。
- 本文件是**实现契约**：编译器只需满足 §3/§5 的公式与 §7 的 IR 映射，
  即可保证本文件 §9 的全部兼容性结论。
