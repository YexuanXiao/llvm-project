// This is a mock file for <coroutine>.
#pragma once

namespace std {

template <typename R, typename...> struct coroutine_traits {
  using promise_type = typename R::promise_type;
};

template <typename Promise = void> struct coroutine_handle;

template <class Promise>
concept __coro_cancellation_aware = requires(Promise &p) {
  p.unhandled_cancellation();
};

template <> struct coroutine_handle<void> {
  static coroutine_handle from_address(void *addr) noexcept {
    coroutine_handle me;
    me.ptr = addr;
    return me;
  }
  void operator()() { resume(); }
  void *address() const noexcept { return ptr; }
  void resume() const { __builtin_coro_resume(ptr); }
  void destroy() const { __builtin_coro_destroy(ptr); }
  bool done() const { return __builtin_coro_done(ptr); }
  void request_cancel() const { __builtin_coro_request_cancel(ptr); }
  bool cancel_requested() const { return __builtin_coro_cancel_requested(ptr); }
  coroutine_handle &operator=(decltype(nullptr)) {
    ptr = nullptr;
    return *this;
  }
  coroutine_handle(decltype(nullptr)) : ptr(nullptr) {}
  coroutine_handle() : ptr(nullptr) {}
//  void reset() { ptr = nullptr; } // add to P0057?
  explicit operator bool() const { return ptr; }

protected:
  void *ptr;
};

template <typename Promise> struct coroutine_handle : coroutine_handle<> {
  using coroutine_handle<>::operator=;

  static coroutine_handle from_address(void *addr) noexcept {
    coroutine_handle me;
    me.ptr = addr;
    return me;
  }

  Promise &promise() const {
    return *reinterpret_cast<Promise *>(
        reinterpret_cast<char *>(ptr) +
        __builtin_coro_promise_v2(alignof(Promise), __coro_cancellation_aware<Promise>));
  }
  static coroutine_handle from_promise(Promise &promise) {
    coroutine_handle p;
    p.ptr = reinterpret_cast<char *>(&promise) -
            __builtin_coro_promise_v2(alignof(Promise), __coro_cancellation_aware<Promise>);
    return p;
  }
  void request_cancel() const { __builtin_coro_request_cancel(ptr); }
  bool cancel_requested() const { return __builtin_coro_cancel_requested(ptr); }
};

template <typename _PromiseT>
bool operator==(coroutine_handle<_PromiseT> const &_Left,
                coroutine_handle<_PromiseT> const &_Right) noexcept {
  return _Left.address() == _Right.address();
}

template <typename _PromiseT>
bool operator!=(coroutine_handle<_PromiseT> const &_Left,
                coroutine_handle<_PromiseT> const &_Right) noexcept {
  return !(_Left == _Right);
}

struct noop_coroutine_promise {};

template <>
struct coroutine_handle<noop_coroutine_promise> {
  operator coroutine_handle<>() const noexcept {
    return coroutine_handle<>::from_address(address());
  }

  constexpr explicit operator bool() const noexcept { return true; }
  constexpr bool done() const noexcept { return false; }

  constexpr void operator()() const noexcept {}
  constexpr void resume() const noexcept {}
  constexpr void destroy() const noexcept {}

  noop_coroutine_promise &promise() const noexcept {
    return *static_cast<noop_coroutine_promise *>(
        __builtin_coro_promise(this->__handle_, alignof(noop_coroutine_promise), false));
  }

  constexpr void *address() const noexcept { return __handle_; }

private:
  friend coroutine_handle<noop_coroutine_promise> noop_coroutine() noexcept;

  coroutine_handle() noexcept {
    this->__handle_ = __builtin_coro_noop();
  }

  void *__handle_ = nullptr;
};

using noop_coroutine_handle = coroutine_handle<noop_coroutine_promise>;

inline noop_coroutine_handle noop_coroutine() noexcept { return noop_coroutine_handle(); }

struct suspend_always {
  bool await_ready() noexcept { return false; }
  void await_suspend(coroutine_handle<>) noexcept {}
  void await_resume() noexcept {}
};
struct suspend_never {
  bool await_ready() noexcept { return true; }
  void await_suspend(coroutine_handle<>) noexcept {}
  void await_resume() noexcept {}
};

} // namespace std
