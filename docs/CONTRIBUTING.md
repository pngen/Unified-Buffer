# Contributing

Thank you for contributing to Unified Buffer. This document explains how to build, test, and submit changes, and the coding
conventions the project enforces.

---

## Building

Use CMake. The library target is `unified_buffer` (alias `UnifiedBuffer::unified_buffer`).

### Windows (validated platform)

From a Developer Command Prompt for Visual Studio, in the repository root:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_CONFIGURATION_TYPES=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

### Linux (intent)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The checked-in source uses MSVC-specific host allocation and Windows implementations of the shared/file backends, so the
validated platform for this release is Windows; Linux builds are for intent and can be augmented with an equivalent toolchain.

### CMake options

| Option | Default | Meaning |
| --- | --- | --- |
| `UNIFIED_BUFFER_ENABLE_CUDA` | `ON` | Probe for the CUDA Toolkit and enable device/pinned domains when found. |
| `UNIFIED_BUFFER_BUILD_TESTS` | `ON` | Build the CTest test targets. |
| `UNIFIED_BUFFER_BUILD_EXAMPLES` | `ON` | Build example targets. |
| `UNIFIED_BUFFER_BUILD_BENCHMARKS` | `ON` | Build benchmark targets. |
| `UNIFIED_BUFFER_WARNINGS_AS_ERRORS` | `ON` | Promote warnings to errors. |

---

## Running tests

```powershell
ctest --test-dir build -C Release --output-on-failure
```

Each `test_*.cpp` registerd under `tests/` is a separate CTest target. Add a new test by dropping a `test_<name>.cpp` into
[`tests/`](../tests/) that defines `main()` and returns `ubtest::report("name")`; the glob builds and registers it
automatically. See [TESTING.md](TESTING.md).

To run a single test target directly, invoke the built executable:

```powershell
build\Release\tests\test_smoke.exe
```

---

## Coding conventions

These are enforced by the build (in CI) and by review. A change that violates them will not be merged.

### Language

- **C++20.** `-std=c++20` (`CMAKE_CXX_STANDARD 20`, `CMAKE_CXX_STANDARD_REQUIRED ON`, `CMAKE_CXX_EXTENSIONS OFF`).
- Use erasable syntax only where the project allows it; the public headers define the API and are C++20.

### No warnings

- `UNIFIED_BUFFER_WARNINGS_AS_ERRORS` (`ON` by default) turns `/WX` on MSVC and `-Werror` elsewhere. The compile flags are
  `/W4 /utf-8 /permissive- /Zc:__cplusplus` on MSVC and `-Wall -Wextra -Wpedantic` otherwise. **Your code must compile warning-free**
  under these flags. Do not suppress a warning globally to make it build; fix the root cause.

### RAII and ownership

- Use RAII for all resources. Public handles (`BufferHandle`, `BufferLease`, `BufferMap`, `BufferView`) are **move-only** and
  RAII: a buffer or lease is released when the object goes out of scope or on an explicit `release()`.
- **Never introduce manual ownership ambiguity.** If you add a type that owns a resource, give it a well-defined `Ownership`
  (from [core/types.hpp](../include/unified_buffer/core/types.hpp)) or a clearly documented owner, and make it move-only unless
  copying is semantically intended. Avoid owning raw pointers that are released elsewhere.
- Prefer `std::unique_ptr` / `std::shared_ptr` over manual new/delete in the implementation.

### Checked arithmetic

- Every size, offset, and alignment computation on a metadata path must use
  [checked_math.hpp](../include/unified_buffer/core/checked_math.hpp) (`add_overflow`, `mul_overflow`, `align_up`,
  `align_down`, `is_aligned`, `range_in_bounds`). **Raw unchecked arithmetic is not permitted on metadata paths.**
- Validate a request before allocating: reject zero and overflow sizes, bad alignments, and out-of-bounds ranges with the
  appropriate `ErrorCode`.

### Errors, not booleans

- Fallible operations return `Result<T>` / `Status`, never a raw boolean. Add a specific `ErrorCode` for a new failure mode
  (see [error.hpp](../include/unified_buffer/core/error.hpp)); use `unknown` only as a last resort.

### Capabilities are explicit

- A new backend must declare its `Capabilities` accurately and must not silently emulate an unsupported operation. The runtime
  rejects unsupported operations with `unsupported_capability` *before* side effects.

### Backend contract

- Implement `IDomain` fully. `export_handle` / `import_handle` may keep the default (unsupported) for domains that do not
  expose an external handle, but every other virtual must be implemented and must validate its inputs (size, bounds,
  alignment) before acting.

### Threading

- Per-buffer mutations are serialized by `Control::mtx`; registry lookups use the shared lock; pool and namespace accounting
  use their own mutexes. Keep lock ordering shallow and do not hold a per-control lock across a long backend call where you can
  avoid it. If you add a new shared structure, document its synchronization discipline.

### Documentation

- Public headers are the API contract. Add a one-line comment for any new public type or function and keep it accurate. If you
  change behaviour, update the relevant file under [`docs/`](../docs/) so the documentation stays truthful.

---

## Commit and review process

1. **Branch.** Work on a short-lived branch off `main`.
2. **Style.** Match the surrounding code: same brace style, naming (PascalCase types, snake_case functions/members), and
   include order.
3. **Tests.** Add or update tests for any behaviour you change. A change without tests (or with failing tests) will not merge.
4. **Warnings.** Build with the default `UNIFIED_BUFFER_WARNINGS_AS_ERRORS`. A warning is a hard failure.
5. **Commit.** Keep commits small and focused; write a clear message that states the behaviour change.
6. **Pull request.** Open a PR against `main`. Describe the change, the motivation, and how you tested it.

### Review criteria

- Does the change respect the documented semantics (identity, generation, ownership, capacity, lease, mapping, pool, migration)?
- Does it use checked arithmetic and structured errors on all new paths?
- Is ownership explicit and free of double-free / use-after-free risk?
- Does it build warning-free on the validated platform?
- Are there tests, and do they pass under `ctest`?
- Is the documentation updated to match the actual behaviour?

---

## License

By contributing you agree that your contributions are licensed under the [Apache-2.0 license](../LICENSE).
