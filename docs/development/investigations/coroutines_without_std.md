# Coroutines without `<coroutine>`

**Context.** The [coroutine proposal](../proposals/coroutines.md) assumes C++20
and includes `<coroutine>`, gated on `__cpp_impl_coroutine`. This note answers a
narrower question that the proposal leaves open: how much of that machinery can
be written by hand, and what does the language force us to keep. The findings
below come from building the core of it (handle, suspend primitives, lazy task,
awaiter) against clang 17 and gcc 14/16 on arm64-darwin, not from reading the
standard.

**Status.** Answered, not implemented. Nothing in the tree uses this yet.

## Summary

Two answers, one inconvenient and one liberating.

**You cannot avoid `std::` entirely.** Both compilers hard-error unless
`std::coroutine_traits` and `std::coroutine_handle` are *defined* in the
translation unit, so "no std::" is not achievable as literally stated. What *is*
achievable: never include `<coroutine>`, use no std type in any interface, and
confine one quarantined `namespace std` block to a single file.

**You never own the frame either way.** The compiler emits the coroutine frame
and hands you an opaque `void*`. "Implement my own coroutine frame" is not
available on this path — the promise only lives *inside* the frame it does not
get to lay out. Owning the frame means stackful coroutines, which is a different
design with its own costs.

So the achievable goal is: own the handle, the suspend primitives, the promise,
and the allocation policy; inherit the frame.

## What the compilers require

Verified against clang 17.0.0, gcc 14, and gcc 16.

| Requirement | Detail |
|-------------|--------|
| `std::coroutine_traits` | Must be **defined**, not merely available. Clang: *"std::coroutine_traits type was not found; include `<coroutine>` before defining a coroutine"*. |
| `std::coroutine_handle` | Must be **defined**. Clang: *"std::coroutine_handle type was not found"*. |
| `std::coroutine_handle<void>` | GCC instantiates it and uses its **members** (`incomplete type 'std::coroutine_handle<void>' used in nested name specifier`). An empty forward declaration is not enough. |
| `<coroutine>` header | Not required. The keywords lower without it. |
| Frame introspection | `__builtin_coro_done`, `__builtin_coro_resume`, `__builtin_coro_destroy`. No portable alternative; both compilers provide all three. |

The promise lookup and the handle-type requirement are the two that cannot be
worked around. Everything else in the standard header is conveniences.

## Traps

These are ordered roughly by how likely they are to cost an afternoon.

### The promise is not at the frame's base address

`get_return_object()` is a promise member function, so the obvious way to build a
handle there is `std::coroutine_handle<Promise>::from_address(this)`. That is
wrong. Measured on both clang 17 and gcc 14, the promise sits at a **non-zero
offset — 16 bytes — inside the frame**:

```
  real frame =0x600002299200
  promise    =0x600002299210  (offset 16)
```

So that handle points into the middle of the frame. Resuming it does not run the
coroutine, and nothing complains. There is a compiler builtin that converts a
promise address to a frame address (`__builtin_coro_promise(p, align, true)`,
which is what libc++ and libstdc++ use for `from_promise`), but it is
clang/gcc-specific and reintroduces exactly the compiler coupling this exercise
is trying to avoid.

The portable design: `get_return_object` returns an object holding a
`Promise*`, and the real frame address is captured in **`initial_suspend`'s
`await_suspend`** — the first place the compiler hands over a frame address, and
it runs before the caller can observe the return object. This is the single
decision the rest of the design hangs off.

### The typed-to-untyped handle conversion must be public

The compiler passes `std::coroutine_handle<Promise>` to an `await_suspend` that
asked for `std::coroutine_handle<>`, and it emits that conversion itself, in code
we do not control. A private converting constructor — the natural reading of
`[coroutine.handle.conv]` — leaves the compiler's own generated code with no
access, and the error surfaces at the `co_await` expression, not in the header.

libc++ does it with a public conversion operator on the typed specialization.
Mirror that. Symmetric transfer is a second place the two kinds meet, since one
awaiter may return a typed handle while the next takes an untyped one.

### `await_ready` and `await_resume` cannot be templates

The compiler invokes them on a concrete awaiter object with no arguments, so a
member template has nothing to deduce from and the call fails. `await_suspend`
*can* be a template, because its handle parameter supplies the deduction context.
That is a useful trick: a shared awaiter can accept any promise type without
naming it.

### `coroutine_traits` specializations must precede the first coroutine

The primary template is only declared, never defined. The first coroutine
compiled in a TU whose return type has no specialization is an error, so the
specializations go at the top of the header, above any coroutine definition.

### Don't constrain the specialization with `enable_if`

An `enable_if` in the pattern is a non-deduced context, so the partial
specialization never matches and the error reverts to the confusing
"no member named `promise_type`". Matching on the return type alone is
sufficient — a foreign type is never our `Task<T>`, so it cannot be captured by
accident.

### The promise is constructed from the coroutine's parameters

As lvalues referring to the parameter objects, so the promise constructor's
signature has to accept the coroutine's parameter list. A move-only parameter
taken by value fails with a deleted-copy error; take it by reference.

### Never transfer to a null handle

`std::noop_coroutine()` cannot be fabricated portably — it needs a frame that the
compiler built and that never runs to completion. It is only needed for a task
that finishes with no continuation, i.e. fire-and-forget. Substitute a helper
coroutine whose `final_suspend` awaiter destroys **its own** frame: the void
`await_suspend` hands control back to the resumer with the frame already gone, so
the finishing task can transfer to the helper, the helper can delete itself, and
the stack unwinds to whoever resumed the task.

## Files

Four new files and one edit to an existing one. Nothing else in the tree changes,
and all of it compiles to nothing on the default C++14 build.

| File | Action | Contents |
|------|--------|----------|
| `dispenso/detail/coroutine_handle.h` | **Edit** (stub exists, untracked) | `namespace std` shim, `CoroutineHandle`, `SuspendAlways`, `SuspendNever` |
| `dispenso/detail/coroutine_task.h` | Create | Detach helper, `TaskPromiseBase`, awaiters, `TaskPromise`, `Task`, `TaskAwaiter`, traits |
| `dispenso/detail/coroutine_sync_wait.h` | Create | `SyncWaitState`, `SyncWaitPromise`, `SyncWaitEntry`, `syncWaitEntry`, `sync_wait`, traits |
| `dispenso/coroutine.h` | Create | Public umbrella; re-exports `Task` and `sync_wait` into `dispenso` |
| `tests/coroutine_test.cpp` | Create | gtest coverage |
| `tests/CMakeLists.txt` | **Edit** | `cxx_std_20` gate for the coroutine test target |

`dispenso/detail/` rather than the proposal's `dispenso/coroutine/` split: the
proposal's layout puts the internals in public headers, and the handle shim in
particular should not be reachable by a plain include. `dispenso/coroutine.h` is
the only public entry point. Not adding these to `dispenso/dispenso.h` keeps the
umbrella C++14-clean; that is a deliberate choice, and it means a user has to ask
for coroutines explicitly.

Naming: the stub already names the class `CoroutineHandle`, so fill that in
rather than renaming it. Free functions are snake_case to match `for_each` and
`parallel_for` — `sync_wait`, not `syncWait`. Everything internal stays in
`dispenso::detail`; only `Task` and `sync_wait` are re-exported.

## Implementation order

### 1. `dispenso/detail/coroutine_handle.h`

Everything in this file is either forced by the compilers or trivially small.
Order within the file matters.

**Delete `~CoroutineHandle()` from the existing stub.** A handle must not
destroy the frame; whoever owns the frame calls `destroy()`. The stub's copy
operations are fine to keep — a handle is just a pointer.

Then, in this order:

1. Unconditional `#error` on `_LIBCPP_COROUTINE` / `_GLIBCXX_COROUTINE`, placed
   *before* the feature guard so a mix-up is caught in C++14 builds too.
2. `#if defined(__cpp_impl_coroutine) && __cpp_impl_coroutine >= 201902L` →
   `#define DISPENSO_HAS_COROUTINES 1`. This is the same macro the proposal uses.
3. `#include <cstddef>` only. `std::swap` is hand-rolled in `swap()` and nothing
   else here needs the library.
4. `namespace std`: forward-declare the `coroutine_traits` primary (declaration
   only, never define it) and the `coroutine_handle` primary.
5. `std::__dispenso_coroutine::coroutine_frame` — a non-template base, not CRTP.
   Public: `address`, `operator!`, `explicit operator bool`, `done`, `operator()`,
   `resume`, `destroy`. The last three are the intrinsics. Protected: a `void*`
   ctor and the `ptr_` member.
6. `std::coroutine_handle<void>` — default ctor, `from_address`,
   `operator=(nullptr_t)`, `swap`; private `void*` ctor.
7. `std::coroutine_handle<Promise>` — same plus `operator=(const coroutine_handle&)`,
   `promise()`, and the **public** `operator coroutine_handle<void>()`.
8. `dispenso::detail::CoroutineHandle<Promise = void>` — the repo's own wrapper.
   Implicit ctor from and conversion operator to `std::coroutine_handle<Promise>`,
   plus `from_address`, `address`, `operator!`, `explicit operator bool`, `done`,
   `resume`, `destroy`, `swap`, and `promise()`. `promise()`'s return type has to
   be a defaulted template parameter, or instantiating the class eagerly forms the
   invalid type `void&` for `CoroutineHandle<void>`.
9. `dispenso::detail::SuspendAlways` and `SuspendNever` — three members each,
   `await_suspend` taking `std::coroutine_handle<>`.

Get this file compiling standalone before writing anything else; it is the only
part that is compiler-coupled.

### 2. `dispenso/detail/coroutine_task.h`

The intra-file order below is load-bearing, not stylistic. Two of these orderings
were found by compiling, not by reading the standard.

1. Forward declarations: `Task<T = void>`, `TaskAwaiter<T, bool IsVoid>`,
   `TaskPromise<T>`, and the `TaskPromise<void>` specialization. Default template
   arguments go **here** and not on the later definitions.
2. The `std::coroutine_traits` specializations for `Task<T>` and for the detach
   entry type, in a `namespace std` block, above every coroutine in the file.
3. **The detach helper, before the awaiters.** `DetachPromise` (with a nested
   `InitialAwaiter` that records the frame address and a nested `FinalAwaiter`
   that calls `destroy()` on itself), `DetachEntry` holding a `DetachPromise*`,
   the `detachEntry()` coroutine, and `makeDetachContinuation()` returning
   `std::coroutine_handle<>`. `get_return_object` is declared in the class and
   defined after `DetachEntry`, since the class is not a template and its body is
   compiled immediately. This block must precede step 4 because
   `TransferToContinuationAwaiter::await_suspend` names `makeDetachContinuation`,
   and a non-dependent name is looked up at definition, not at instantiation.
4. `TaskPromiseBase` — non-template, so both promise specializations share it.
   Public: `frameAddress`, `setFrameAddress`, `handle` (untyped), `continuation`,
   `setContinuation`, `exception`, `unhandled_exception`, `initial_suspend`,
   `final_suspend`. Private: `frame_`, `continuation_` (a `CoroutineHandle<>`),
   `exception_`.
5. `CaptureFrameAwaiter` and `TransferToContinuationAwaiter`. In both, only
   `await_suspend` is a template. The second returns the continuation, or
   `makeDetachContinuation()` when there is none.
6. `TaskPromise<T>` : `TaskPromiseBase` — `get_return_object` (declared only),
   `return_value`, `value()`; private `std::optional<T> value_`. Then
   `TaskPromise<void>` : `TaskPromiseBase` — `get_return_object`, `return_void`.
7. `Task<T>` — `promise_type` and `value_type` aliases, `static_assert` against
   reference `T`, move ctor and move assign that null the source, deleted copy,
   destructor calling `destroy()`, `explicit operator bool`, `detach()`,
   `operator co_await() &&` declared only. Private: the `promise_type*` ctor,
   `destroy`, `handle`, `promise`, `promise_`. Friends: `TaskAwaiter` (needs
   `handle` and `promise`) and `TaskPromise<T>` (needs the pointer ctor).
8. `TaskAwaiter<T, IsVoid>` — the value form (`await_resume` returns `T`) and the
   `<T, true>` specialization for void. Both hold a `Task<T>` by value, so the
   task is consumed whether or not the await completes.
9. Out-of-line definitions: `Task<T>::operator co_await`, both
   `get_return_object` bodies, and the detach `get_return_object`.

### 3. `dispenso/detail/coroutine_sync_wait.h`

1. Forward declarations, then the `std::coroutine_traits` specialization for
   `SyncWaitEntry<T>`, above the coroutine in this file.
2. `SyncWaitState<T>` — `setValue`, `take`, `setException`, `rethrowIfFailed`,
   `event`; private `std::optional<T>`, `exception_ptr`, `CompletionEvent`. Plus
   the `void` specialization without the value members.
3. `SyncWaitPromise<T>` : `TaskPromiseBase` — constructor taking
   `(Task<T>&, SyncWaitState<T>&)`, because a promise is built from the
   coroutine's parameters and `Task` is move-only. Plus `get_return_object`,
   `return_void`, `unhandled_exception` forwarding to the state, `final_suspend`
   returning a nested `FinalAwaiter` that notifies the event and then destroys
   *its own* frame, and a `resume()` that goes through the inherited `handle()`.
4. `SyncWaitEntry<T>` — `promise_type` alias and a `SyncWaitPromise<T>*`.
5. `syncWaitEntry(Task<T>, SyncWaitState<T>&)`, one function for both arities
   using `if constexpr` on `is_same<T, void>`. An explicit specialization of the
   function template does not work here: the primary is ill-formed for `void` and
   the compiler reports the specialization as a substitution failure.
6. `sync_wait(Task<T>)` and the `void` overload — construct the state, drive
   `entry.promise->resume()`, `state.event().wait()`, rethrow, take.

### 4. `dispenso/coroutine.h`

Includes the three detail headers and re-exports `Task` and `sync_wait` into
`dispenso`. Keep the `@ingroup group_coroutine` doxygen block the other headers
use so this shows up in the docs.

### 5. `tests/coroutine_test.cpp`

Caught by the `*test.cpp` glob, so it needs no CMake file entry — only the
standard gate in step 6. Wrap the whole file in `#if defined(DISPENSO_HAS_COROUTINES)`
so it builds to an empty binary on C++14. Cover, in increasing difficulty:

- a coroutine that does not run its body until awaited (laziness)
- a non-void task, a `void` task, and a task returning a move-only type
- move semantics: a moved-from task is falsy and destroying it is a no-op
- `co_await` nesting three deep, and a task that is never awaited (the frame is
  destroyed, the body never runs)
- an exception thrown in a task, caught at the await point, with the message
  intact
- `sync_wait` on a value, on void, and on a throwing task
- `detach()` on a task that runs to completion
- a 100k-deep `co_await` chain, which overflows the stack immediately if
  symmetric transfer is not actually happening

### 6. `tests/CMakeLists.txt`

The `package_add_test` macro does not set a standard, and the `dispenso` target
exports `cxx_std_14`, so the coroutine test compiles as C++14 and the guard makes
it empty. Add a `check_cxx_source_compiles` probe for a coroutine with
`CMAKE_REQUIRED_STANDARD 20`, and if it passes apply
`target_compile_features(coroutine_test PRIVATE cxx_std_20)` after the existing
test loop. Gate it so a compiler without coroutine support still configures.

### 7. Hardening

A never-awaited task is genuinely free — no body runs, and its locals are not
destroyed — so cancellation of a task suspended mid-body needs its own decision
and its own test. Then: an exception thrown inside a final awaiter (currently
`noexcept`, so it terminates), ASan for the self-destroying frames, TSan for a
task finished by a different thread than the one that created it, and the
deep-chain test above under ASan.

## Build integration notes

Details that apply across all of the above rather than to one step.

- Headers are globbed as `*.h` and installed by the same glob, so a new header is
  picked up and shipped without a CMake edit. That is convenient and also means a
  header cannot be left out of the install by accident.
- Test targets build with `-Wall -Wextra -pedantic -Wconversion -Werror`, so the
  headers have to be clean at that level, not just at `-Wall`. The frame
  intrinsics and the pointer plumbing are where a conversion warning would
  surface.
- gcc additionally needs `-Wno-stringop-overflow -Wno-maybe-uninitialized` in
  places, per the existing targets; coroutine frames give both warnings false
  positives.
- Mixing this with the real `<coroutine>` in one TU is a redefinition error. The
  `#error` guard in step 1 catches it, but verify first that gtest/gmock do not
  pull `<coroutine>` in transitively — if they do on some toolchain, the guard
  turns a working build into a hard failure.
- The proposal states the machinery will not be backported below C++20. Every step
  here compiles fine as dead code on a C++14 build, so that is a policy choice
  about maintenance, not a technical limit.

## What was not verified

The core compiled clean at `-Wall -Wextra -pedantic -Wconversion -Werror` and
started lazily as intended. Beyond that, runtime behaviour was not confirmed:
`sync_wait` crashed on a null frame address whose cause was not diagnosed, and
`co_await` nesting, exception propagation, detached tasks, and the deep-chain
stack-flatness claim were never exercised. Treat the ordering above as the shape
of the work, not as a working reference implementation.

## Open questions for the proposal

- The proposal's `__cpp_impl_coroutine` guard exists to detect C++20. If the
  handle shim is ever wanted independently of the keywords, it needs its own
  feature test.
- `Task` needs a `detach()`-style escape hatch for fire-and-forget, which is
  what forces the noop substitute. If fire-and-forget is dropped, the detach
  helper in step 2 disappears and `final_suspend` can assume a continuation.
- Whether `await_suspend` should ever be allowed to throw. Every path here is
  `noexcept`, which means frame allocation failure inside the detach helper
  terminates rather than propagating.
