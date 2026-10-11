# ADR 0002: Google C++ style and error handling

## Status

Accepted

## Context

The Week 1 code reported errors as raw `int` errno values, with factories
taking an `int* error` output parameter. The project wants to follow the
Google C++ Style Guide throughout, and the guide prefers return values over
output parameters. It also targets C++20, which has no standard "value or
error" type (`std::expected` is C++23).

A brief move to C++23 + LLVM 19 for `std::expected` was reverted: it
deviated from the guide's C++ version rule and needed a non-default
toolchain on Ubuntu 24.04.

## Options

- Option A: return `std::error_code`, with a `std::error_code&` parameter on
  functions that also produce a value (the `std::filesystem` non-throwing
  idiom). Keeps output parameters.
- Option B: a small project-local `Result<T>` with the same interface as a
  subset of `std::expected<T, std::error_code>`.
- Option C: adopt Abseil's `absl::StatusOr`, which is what Google code uses.

## Decision

Option B. `src/result.h` defines `sluice::Result<T>` (a `std::variant` of
the value and a `std::error_code`), `Result<void>`, and `sluice::Error`, the
failure value. Functions return either a value or `Error(...)`; there are no
output parameters. The interface (`has_value`, `operator bool`, `*`, `->`,
`error()`) matches `std::expected`, so a later move to C++23 only changes
that header. Misuse (reading the value of a failed Result) aborts.

The code follows the Google C++ Style Guide for C++20: `#define` header
guards named `SLUICE_<PATH>_<FILE>_H_`, Google include order (enforced by
`.clang-format`), an 80-column limit, includes instead of forward
declarations wherever there is no include cycle, and explicit lambda
captures for callbacks that outlive their scope. clang-tidy enables the
`google-*` checks. Logging uses `fprintf`, which the guide allows; string
building uses C++20 `std::format`.

The toolchain is Ubuntu 24.04's default LLVM 18, pinned by package name
(`clang-18`, `clang-format-18`, `clang-tidy-18`) in the devcontainer, CI,
CMake presets, and scripts.

## Trade-offs

- `Result<T>` is one more file to understand and test (`tests/result_test.cpp`),
  and lacks `std::expected`'s monadic operations (`and_then`, `transform`),
  so error propagation is written as explicit early returns.
- Its implicit converting constructors (needed for `return value;`) are
  marked `NOLINT(google-explicit-constructor)`, as the guide allows for
  interchangeable types.
- `std::error_code` carries no context string (e.g. which address failed to
  bind); callers add that context when they report the error.
- Option C was rejected: Abseil is a large dependency for one type.

## References

- Google C++ Style Guide: "Inputs and Outputs", "C++ Version",
  "The #define Guard", "Names and Order of Includes", "Implicit
  Conversions", "Lambda Expressions"
- `src/result.h`, `tests/result_test.cpp`
