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
    T value{};

    Task get_return_object() {
      return Task(std::coroutine_handle<promise_type>::from_promise(*this));
    }

    // Lazy start; the body runs only when the task is awaited.
    std::suspend_always initial_suspend() noexcept {
      return {};
    }

    // Keep the frame alive until Task::await_resume has read the result.
    std::suspend_always final_suspend() noexcept {
      return {};
    }

    void return_value(T v) {
      value = std::move(v);
    }

    void unhandled_exception() {
      std::terminate();
    }
  };

  explicit Task(std::coroutine_handle<promise_type> handle) : handle_m{handle} {}

  Task(Task&& obj) noexcept : handle_m{std::exchange(obj.handle_m, {})} {}

  ~Task() {
    if (handle_m) {
      handle_m.destroy();
    }
  }

  bool await_ready() const noexcept {
    return false;
  }

  void await_suspend(std::coroutine_handle<>) {
    handle_m.resume();
  }

  T await_resume() {
    return std::move(handle_m.promise().value);
  }

 private:
  std::coroutine_handle<promise_type> handle_m;
};

// TODO: exception propagation, proper move semantics, continuation/symmetric transfer, detach.

#endif // defined(__cpp_impl_coroutine) && __cpp_impl_coroutine >= 201902L
