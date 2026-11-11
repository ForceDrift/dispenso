/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * @file coroutine_task.h
 * @ingroup group_coroutine
 * A lazy C++20 coroutine task type.
 **/

#pragma once

#if defined(__cpp_impl_coroutine) && __cpp_impl_coroutine >= 201902L

#include <coroutine>
#include <exception>
#include <utility>

template <typename T>
class Task {
 public:
  struct promise_type {
   public:
    T value{};

    promise_type() noexcept {}
    ~promise_type() {}

    Task get_return_object() noexcept {
      return Task(std::coroutine_handle<promise_type>::from_promise(*this));
    }

    // Lazy start; the body runs only when the task is awaited.
    std::suspend_always initial_suspend() noexcept {
      return {};
    }

    struct final_awaitable {
      bool await_ready() const noexcept {
        return false;
      }

      // Symmetric transfer back to the awaiting coroutine, completing the
      // chain started by awaiter::await_suspend. With a null continuation
      // (task driven from outside a coroutine), remain suspended so the
      // driver can read the result before ~Task destroys the frame.
      std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
        std::coroutine_handle<> continuation = h.promise().continuation_m;
        return continuation ? continuation : std::noop_coroutine();
      }

      void await_resume() const noexcept {}
    };

    // Keep the frame alive until Task::await_resume has read the result.
    final_awaitable final_suspend() noexcept {
      return {};
    }

    void return_value(T v) {
      value = std::move(v);
    }

    // Capture the exception into the promise instead of terminating; the
    // parent rethrows it from await_resume, mirroring FutureImplResultMember
    // in detail/future_impl.h. Must not throw itself.
    void unhandled_exception() noexcept {
#if defined(__cpp_exceptions)
      exception_ = std::current_exception();
#else
      std::terminate();
#endif
    }

    // Always present to ensure stable ABI layout regardless of __cpp_exceptions.
    std::exception_ptr exception_;

    // The coroutine awaiting this task; resumed by final_awaitable when the
    // body completes or throws.
    std::coroutine_handle<> continuation_m{};
  };

  explicit Task(std::coroutine_handle<promise_type> handle) : handle_m{handle} {}

  // A Task uniquely owns its coroutine frame; copies are forbidden.
  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;

  Task(Task&& obj) noexcept : handle_m{std::exchange(obj.handle_m, {})} {}

  // Steal the other frame (destroying ours first); the source is left null
  // so its destructor is a no-op.
  Task& operator=(Task&& obj) noexcept {
    if (this != &obj) {
      if (handle_m != nullptr) {
        handle_m.destroy();
      }
      handle_m = std::exchange(obj.handle_m, {});
    }
    return *this;
  }

  ~Task() {
    if (handle_m != nullptr) {
      handle_m.destroy();
    }
  }

  // Awaiter handed back by operator co_await; uses symmetric transfer so
  // awaiting a task never grows the stack.
  struct awaiter {
    explicit awaiter(std::coroutine_handle<promise_type> h) noexcept : coro_(h) {}

    // Ready only if there is no frame to run or it already ran.
    bool await_ready() noexcept {
      return !coro_ || coro_.done();
    }

    // Suspend the awaiting coroutine and transfer control to the task; the
    // awaiting handle is stashed in the promise so final_awaitable can resume
    // it when the task finishes.
    std::coroutine_handle<promise_type> await_suspend(std::coroutine_handle<> h) noexcept {
      coro_.promise().continuation_m = h;
      return coro_;
    }

    // Propagate a captured exception (if any) instead of returning the
    // (default-constructed, never-set) value.
    int await_resume() {
#if defined(__cpp_exceptions)
      if (coro_.promise().exception_) {
        std::rethrow_exception(coro_.promise().exception_);
      }
#endif
      return std::move(coro_.promise().value);
    }

   private:
    std::coroutine_handle<promise_type> coro_;
  };

  awaiter operator co_await() noexcept {
    return awaiter(handle_m);
  }

 protected:
  std::coroutine_handle<promise_type> handle_m;
};

// TODO: symmetric transfer

#endif // defined(__cpp_impl_coroutine) && __cpp_impl_coroutine >= 201902L
