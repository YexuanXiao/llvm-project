# 构建与复现指南

本提案（协作取消 / PxxxxR0）的完整实现横跨 LLVM 编译器、Clang 前端与
libc++ 标准库。本文档说明如何构建修改后的工具链、配置 libc++，以及
复现性能基准（`benchmark/`）。

目录：

1. [概述](#概述)
2. [构建 LLVM + Clang（含修改）](#构建-llvm--clang含修改)
3. [构建 libc++（ABI v2）](#构建-libcabi-v2)
4. [功能验证](#功能验证)
5. [运行性能基准](#运行性能基准)
6. [运行测试套件](#运行测试套件)

---

## 概述

源码改动分布在三个仓库区域：

| 区域 | 改动 |
|---|---|
| `llvm/` | 新增 1 个 marker intrinsic `llvm.coro.cancelled`；`CoroShape` 增加取消感知字段（`HasCancel` / `CancelFlagOffset` / `CancelEntryBlock`）；`CoroFrame` 在 promise 后布局 i8 取消标志；`CoroSplit` 在 resume 入口插入取消检查、ramp 中初始化标志、destroy/cleanup 克隆移除取消路径 |
| `clang/` | 新增 2 个 builtin `__builtin_coro_request_cancel` / `__builtin_coro_cancel_requested`（降低为既有 `llvm.coro.promise` + 指针运算，零新增操纵 intrinsic）；`CoroutineBodyStmt` 增加 `OnCancellation` 子语句；Sema 查找 `promise.unhandled_cancellation()`；CodeGen 生成取消入口块（`llvm.coro.cancelled` marker + 调用）与 body 前的取消检查 |
| `libcxx/` | `coroutine_handle<Promise>` 新增 `request_cancel()` / `cancel_requested()`（模板成员，调用 builtin 并传 `alignof/sizeof(Promise)`） |

ABI 兼容性：取消标志位于帧中 promise 对象之后（`frame + 2*ptrsize +
sizeof(Promise)`），resume/destroy 函数指针与 promise 投影偏移不变；未取消
感知的协程帧布局逐字节不变。

## 构建 LLVM + Clang（含修改）

前置依赖：`cmake`、`ninja`、系统 C++ 编译器（如 gcc）。

```bash
cd /home/bizwen/llvm-project

cmake -S llvm -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_PROJECTS=clang \
  -DLLVM_TARGETS_TO_BUILD=X86

# 构建 clang（含 LLVM 协程 passes；并行度按机器核数调整）
cmake --build build --target clang -j6
```

产物：`build/bin/clang`、`build/bin/clang++`、`build/bin/opt`、
`build/bin/llvm-lit`。

> 说明：首次构建时间较长；之后增量构建只需
> `cmake --build build --target clang -j6`。

## 构建 libc++（ABI v2）

libc++ 不参与主构建（`LLVM_ENABLE_RUNTIMES` 为空）。独立构建一份
Release 版 libc++（ABI v2，与 monorepo 头文件的 `__2` 命名空间一致），
供基准与功能测试链接。

```bash
cmake -S libcxx -B build/libcxx-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=/home/bizwen/llvm-project/build/bin/clang \
  -DCMAKE_CXX_COMPILER=/home/bizwen/llvm-project/build/bin/clang++ \
  -DLIBCXX_ENABLE_ABI_VERSION=2 \
  -DLIBCXX_ENABLE_EXCEPTIONS=ON

cmake --build build/libcxx-build -j6
```

产物：

- 库：`build/libcxx-build/lib/libc++.so.2`、`libc++abi.so.1`
- 配置头：`build/libcxx-build/include/c++/v1/__config_site`

> 注意：`LIBCXX_ABI_VERSION=2` 必须与编译测试程序时所用的 `__config_site`
> 一致（见下文），否则链接报 ABI 命名空间不匹配（`std::__1` vs `std::__2`）。

### 测试用 include 配置（`/tmp/coro_test/include`）

基准/功能测试用 monorepo 的 libc++ 头文件（含 `request_cancel` 修改），
但头文件需要 `__config_site`。使用手写最小配置（构建生成的
`__config_site` 与源码头直接组合会因 include 顺序报 `hash.h` 的
`std::memcpy` 解析错误，因此不推荐）：

创建 `/tmp/coro_test/include/__config_site`（与 libcxx 默认 Linux 配置
一致，要点是 ABI v2 与 `__2` 命名空间）：

```cpp
#define _LIBCPP_ABI_VERSION 2
#define _LIBCPP_ABI_NAMESPACE __2
#define _LIBCPP_HAS_THREADS 1
#define _LIBCPP_HAS_MONOTONIC_CLOCK 1
#define _LIBCPP_HAS_THREAD_API_PTHREAD 1
#define _LIBCPP_HAS_FILESYSTEM 1
#define _LIBCPP_HAS_RANDOM_DEVICE 1
#define _LIBCPP_HAS_LOCALIZATION 1
#define _LIBCPP_HAS_UNICODE 1
#define _LIBCPP_HAS_WIDE_CHARACTERS 1
#define _LIBCPP_HAS_TIME_ZONE_DATABASE 0
#define _LIBCPP_HAS_NO_VENDOR_AVAILABILITY_ANNOTATIONS 1
#define _LIBCPP_HARDENING_MODE_DEFAULT _LIBCPP_HARDENING_MODE_NONE
```

另需 `/tmp/coro_test/include/__assertion_handler`：

```cpp
// Minimal stub: libc++ assertions compiled out (hardening mode none).
#include <__assertion_handler>
```

## 功能验证

`proposal_asserts/` 下的功能测试依赖三样东西：
`build/bin/clang++`、`build/libcxx-build/lib`、`/tmp/coro_test/include`。

### 端到端取消行为（`/tmp/coro_test/test_cancel.cpp`）

覆盖：初始挂起后取消、`done()`、中途取消、`cancel_requested()`、
正常恢复回归、中途正常完成回归，共 6 项断言。

```bash
LIB=/home/bizwen/llvm-project/build/libcxx-build/lib
CC=/home/bizwen/llvm-project/build/bin/clang++

$CC -std=c++23 -O2 -nostdinc++ \
  -I /home/bizwen/llvm-project/libcxx/include \
  -I /tmp/coro_test/include \
  /tmp/coro_test/test_cancel.cpp -o /tmp/coro_test/test_cancel \
  -L$LIB -lc++ -lc++abi -Wl,-rpath,$LIB
/tmp/coro_test/test_cancel
# 期望输出：6 项全部 PASS
```

### CWG2934 语义（`/tmp/coro_test/cwg2934_test.cpp`）

验证 `unhandled_cancellation()` 抛出异常时：异常传播给 resumer、
协程标记为 done（非"暂停在 final suspend 点"）、body 不执行、
`destroy()` 释放帧。

```bash
$CC -std=c++23 -O2 -nostdinc++ \
  -I /home/bizwen/llvm-project/libcxx/include \
  -I /tmp/coro_test/include \
  /tmp/coro_test/cwg2934_test.cpp -o /tmp/coro_test/cwg2934_test \
  -L$LIB -lc++ -lc++abi -Wl,-rpath,$LIB
/tmp/coro_test/cwg2934_test
# 期望输出：PASS: exception from unhandled_cancellation marks done and destroy frees frame
```

> 测试用 `noinline` 的 `destroy_handle` 阻止 CoroElide 把帧省略到栈上，
> 否则 `dealloc_count == 0` 是省略的合法结果，不能作为帧泄漏的判据。

## 运行性能基准

基准对比嵌套 generator（`root -> mid -> leaf`）的异常传播：

- `bench_old.cpp`：stock `generator.hpp`（每层 throw/catch）
- `bench_new.cpp`：`generator_improved.hpp`（取消链，源头 throw 一次、
  终点 catch 一次，中间层仅置标志 + 对称转移）
- `bench_depth5.cpp`：深度 5 版本（验证 O(N) vs ≈O(1) 可扩展性）

```bash
cd /home/bizwen/llvm-project/proposal_asserts/benchmark
LIB=/home/bizwen/llvm-project/build/libcxx-build/lib
CC=/home/bizwen/llvm-project/build/bin/clang++

# 深度 3
$CC -std=c++23 -O2 -nostdinc++ \
  -I /home/bizwen/llvm-project/libcxx/include \
  -I /tmp/coro_test/include \
  -I /home/bizwen/llvm-project/proposal_asserts \
  bench_old.cpp -o bench_old -L$LIB -lc++ -lc++abi -Wl,-rpath,$LIB
$CC -std=c++23 -O2 -nostdinc++ \
  -I /home/bizwen/llvm-project/libcxx/include \
  -I /tmp/coro_test/include \
  -I /home/bizwen/llvm-project/proposal_asserts \
  bench_new.cpp -o bench_new -L$LIB -lc++ -lc++abi -Wl,-rpath,$LIB

./bench_old 200000   # stock
./bench_new 200000   # 取消链

# 深度 5（同一编译命令，加 -DUSE_IMPROVED 选实现）
$CC -std=c++23 -O2 -DUSE_IMPROVED -nostdinc++ \
  -I /home/bizwen/llvm-project/libcxx/include \
  -I /tmp/coro_test/include \
  -I /home/bizwen/llvm-project/proposal_asserts \
  bench_depth5.cpp -o bench_d5_new -L$LIB -lc++ -lc++abi -Wl,-rpath,$LIB
./bench_d5_new 200000
```

### 参考结果（Clang 21, -O2, x86-64, rounds=200000）

| 深度 | stock overhead ns/round | 取消链 overhead ns/round | 加速比 |
|---|---|---|---|
| 3 | ≈12,100 | ≈4,700 | ≈2.6× |
| 5 | ≈17,600 | ≈4,900 | ≈3.6× |

关键结论：

- stock 的开销随深度线性增长（每层 ≈2.7µs：一次 `rethrow_exception` +
  一次 catch）；
- 取消链近似恒定（每层 ≈0.25µs：置标志 + 对称转移，尾调用零 throw/catch）；
- 正常路径（不抛异常）两者差异在噪声范围内（取消感知协程每次恢复多一次
  flag load + branch）。

### 正确性自检

两个实现产出的 yield 序列必须一致，且 `sink` 校验和相同：

```bash
# 旧版本 bug：取消链若不用对称转移驱动，传播期间迭代器会暴露残留的
# value_ptr（幽灵值），表现为 sink 不一致。当前实现已修复（final_suspend
# 总是对称转移回父，整条取消链在一次用户 resume 内走完）。
./bench_old 10000   # sink=80000 (depth 3)
./bench_new 10000   # sink=80000（必须相同）
```

## 运行测试套件

```bash
cd /home/bizwen/llvm-project/build

# 新增的 LLVM 层测试（帧布局、resume 入口分支、marker 删除）
./bin/llvm-lit ../llvm/test/Transforms/Coroutines/coro-cancel.ll

# 新增的 Clang 层测试（语义 + IR 生成）
./bin/llvm-lit ../clang/test/SemaCXX/coro-cancel.cpp \
               ../clang/test/CodeGenCoroutines/coro-cancel.cpp

# 既有协程测试全量回归（验证 ABI 兼容，无新增失败）
./bin/llvm-lit ../llvm/test/Transforms/Coroutines -j6
./bin/llvm-lit ../clang/test/CodeGenCoroutines -j6
```

期望：新增测试全部通过；既有测试零新增失败。
