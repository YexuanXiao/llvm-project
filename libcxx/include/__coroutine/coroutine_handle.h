//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef _LIBCPP___COROUTINE_COROUTINE_HANDLE_H
#define _LIBCPP___COROUTINE_COROUTINE_HANDLE_H

#include <__assert>
#include <__config>
#include <__cstddef/nullptr_t.h>
#include <__cstddef/size_t.h>
#include <__functional/hash.h>
#include <__memory/addressof.h>
#include <__type_traits/remove_cv.h>
#include <compare>

#if !defined(_LIBCPP_HAS_NO_PRAGMA_SYSTEM_HEADER)
#  pragma GCC system_header
#endif

#if _LIBCPP_STD_VER >= 20

_LIBCPP_BEGIN_NAMESPACE_STD

// [coroutine.handle]
template <class _Promise = void>
struct coroutine_handle;

template <>
struct coroutine_handle<void> {
public:
  // [coroutine.handle.con], construct/reset
  constexpr coroutine_handle() noexcept = default;

  _LIBCPP_HIDE_FROM_ABI constexpr coroutine_handle(nullptr_t) noexcept {}

  _LIBCPP_HIDE_FROM_ABI coroutine_handle& operator=(nullptr_t) noexcept {
    __handle_ = nullptr;
    return *this;
  }

  // [coroutine.handle.export.import], export/import
  [[nodiscard]] _LIBCPP_HIDE_FROM_ABI constexpr void* address() const noexcept { return __handle_; }

  [[nodiscard]] _LIBCPP_HIDE_FROM_ABI static constexpr coroutine_handle from_address(void* __addr) noexcept {
    coroutine_handle __tmp;
    __tmp.__handle_ = __addr;
    return __tmp;
  }

  // [coroutine.handle.observers], observers
  _LIBCPP_HIDE_FROM_ABI constexpr explicit operator bool() const noexcept { return __handle_ != nullptr; }

  [[nodiscard]] _LIBCPP_HIDE_FROM_ABI bool done() const {
    _LIBCPP_ASSERT_VALID_EXTERNAL_API_CALL(__is_suspended(), "done() can be called only on suspended coroutines");
    return __builtin_coro_done(__handle_);
  }

  // [coroutine.handle.resumption], resumption
  _LIBCPP_HIDE_FROM_ABI void operator()() const { resume(); }

  _LIBCPP_HIDE_FROM_ABI void resume() const {
    _LIBCPP_ASSERT_VALID_EXTERNAL_API_CALL(__is_suspended(), "resume() can be called only on suspended coroutines");
    _LIBCPP_ASSERT_VALID_EXTERNAL_API_CALL(!done(), "resume() has undefined behavior when the coroutine is done");
    __builtin_coro_resume(__handle_);
  }

  _LIBCPP_HIDE_FROM_ABI void destroy() const {
    _LIBCPP_ASSERT_VALID_EXTERNAL_API_CALL(__is_suspended(), "destroy() can be called only on suspended coroutines");
    __builtin_coro_destroy(__handle_);
  }

private:
  _LIBCPP_HIDE_FROM_ABI bool __is_suspended() const {
    // FIXME actually implement a check for if the coro is suspended.
    return __handle_ != nullptr;
  }

  void* __handle_ = nullptr;
};

// [coroutine.handle.compare]
inline _LIBCPP_HIDE_FROM_ABI constexpr bool operator==(coroutine_handle<> __x, coroutine_handle<> __y) noexcept {
  return __x.address() == __y.address();
}
inline _LIBCPP_HIDE_FROM_ABI constexpr strong_ordering
operator<=>(coroutine_handle<> __x, coroutine_handle<> __y) noexcept {
  return compare_three_way()(__x.address(), __y.address());
}

template <class _Promise>
struct coroutine_handle {
public:
  // [coroutine.handle.con], construct/reset
  constexpr coroutine_handle() noexcept = default;

  _LIBCPP_HIDE_FROM_ABI constexpr coroutine_handle(nullptr_t) noexcept {}

  [[nodiscard]] _LIBCPP_HIDE_FROM_ABI static coroutine_handle from_promise(_Promise& __promise) {
    using _RawPromise = __remove_cv_t<_Promise>;
    coroutine_handle __tmp;
    __tmp.__handle_ =
        __builtin_coro_promise(std::addressof(const_cast<_RawPromise&>(__promise)), alignof(_Promise), true);
    return __tmp;
  }

  _LIBCPP_HIDE_FROM_ABI coroutine_handle& operator=(nullptr_t) noexcept {
    __handle_ = nullptr;
    return *this;
  }

  // [coroutine.handle.export.import], export/import
  [[nodiscard]] _LIBCPP_HIDE_FROM_ABI constexpr void* address() const noexcept { return __handle_; }

  [[nodiscard]] _LIBCPP_HIDE_FROM_ABI static constexpr coroutine_handle from_address(void* __addr) noexcept {
    coroutine_handle __tmp;
    __tmp.__handle_ = __addr;
    return __tmp;
  }

  // [coroutine.handle.conv], conversion
  _LIBCPP_HIDE_FROM_ABI constexpr operator coroutine_handle<>() const noexcept {
    return coroutine_handle<>::from_address(address());
  }

  // [coroutine.handle.observers], observers
  _LIBCPP_HIDE_FROM_ABI constexpr explicit operator bool() const noexcept { return __handle_ != nullptr; }

  [[nodiscard]] _LIBCPP_HIDE_FROM_ABI bool done() const {
    _LIBCPP_ASSERT_VALID_EXTERNAL_API_CALL(__is_suspended(), "done() can be called only on suspended coroutines");
    return __builtin_coro_done(__handle_);
  }

  // [coroutine.handle.resumption], resumption
  _LIBCPP_HIDE_FROM_ABI void operator()() const { resume(); }

  _LIBCPP_HIDE_FROM_ABI void resume() const {
    _LIBCPP_ASSERT_VALID_EXTERNAL_API_CALL(__is_suspended(), "resume() can be called only on suspended coroutines");
    _LIBCPP_ASSERT_VALID_EXTERNAL_API_CALL(!done(), "resume() has undefined behavior when the coroutine is done");
    __builtin_coro_resume(__handle_);
  }

  _LIBCPP_HIDE_FROM_ABI void destroy() const {
    _LIBCPP_ASSERT_VALID_EXTERNAL_API_CALL(__is_suspended(), "destroy() can be called only on suspended coroutines");
    __builtin_coro_destroy(__handle_);
  }

  // [coroutine.handle.promise], promise access
  [[nodiscard]] _LIBCPP_HIDE_FROM_ABI _Promise& promise() const {
    return *static_cast<_Promise*>(__builtin_coro_promise(this->__handle_, alignof(_Promise), false));
  }

  // [coroutine.handle.cancel], cancellation
  // The cancellation flag of a cancellation-aware coroutine lives in the
  // coroutine frame right after the promise object. The builtins take
  // `alignof`/`sizeof` of the promise type to locate the flag relative to the
  // promise, which is found via `llvm.coro.promise` (an existing intrinsic).
  // The members are constrained: they exist only if the promise type is
  // cancellation-aware (declares `unhandled_cancellation()`), enforced at
  // compile time by a requires expression; a non-cancellation-aware promise
  // simply has no `request_cancel`/`cancel_requested` members. Preconditions
  // (undefined behavior if violated): the coroutine is suspended; for
  // `request_cancel()`, it is not already cancelled. The frame always
  // reserves the flag byte, so a call that circumvents the constraints stays
  // in bounds, but that is an implementation detail, not a guarantee.
  // `coroutine_handle<void>` does not provide these operations because the
  // type-erased handle does not carry the promise size and alignment.
  _LIBCPP_HIDE_FROM_ABI void request_cancel() const
    requires requires(_Promise& __p) { __p.unhandled_cancellation(); } {
    _LIBCPP_ASSERT_VALID_EXTERNAL_API_CALL(__is_suspended(),
                                           "request_cancel() can be called only on suspended coroutines");
    __builtin_coro_request_cancel(__handle_, alignof(_Promise), sizeof(_Promise));
  }

  [[nodiscard]] _LIBCPP_HIDE_FROM_ABI bool cancel_requested() const
    requires requires(_Promise& __p) { __p.unhandled_cancellation(); } {
    _LIBCPP_ASSERT_VALID_EXTERNAL_API_CALL(__is_suspended(),
                                           "cancel_requested() can be called only on suspended coroutines");
    return __builtin_coro_cancel_requested(__handle_, alignof(_Promise), sizeof(_Promise));
  }

private:
  _LIBCPP_HIDE_FROM_ABI bool __is_suspended() const {
    // FIXME actually implement a check for if the coro is suspended.
    return __handle_ != nullptr;
  }
  void* __handle_ = nullptr;
};

// [coroutine.handle.hash]
template <class _Tp>
struct hash<coroutine_handle<_Tp>> {
  [[nodiscard]] _LIBCPP_HIDE_FROM_ABI size_t operator()(const coroutine_handle<_Tp>& __v) const noexcept {
    return hash<void*>()(__v.address());
  }
};

_LIBCPP_END_NAMESPACE_STD

#endif // _LIBCPP_STD_VER >= 20

#endif // _LIBCPP___COROUTINE_COROUTINE_HANDLE_H
