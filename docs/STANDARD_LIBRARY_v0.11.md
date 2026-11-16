# Serenity Standard Library v0.11 (historical)

The first standard-library release provides a small collection of pure Serenity
math helpers in `stdlib/std/math.snh`.

## Inclusion

```serenity
%include "std/math.snh"
```

The preprocessor searches for quoted includes in this order:

1. Relative to the file containing the `%include` directive.
2. Under Serenity's configured standard-library directory.

The standard-library directory is set to the project's `stdlib/` directory by
CMake. `SERENITY_STDLIB_PATH` is searched first at runtime, with the CMake path
retained as a fallback. The variable must name the directory containing `std/`,
for example:

```sh
export SERENITY_STDLIB_PATH="$HOME/code/serenity/stdlib"
```

Do not point it at the `stdlib/std/` subdirectory: Serenity includes headers
using paths such as `std/math.snh`, which are appended to the configured root.
A literal leading `~/` is also expanded by the preprocessor. This is a single
standard-library root, not general `-I` include-directory support.

## Math functions

Because function overloading is not supported yet, function names carry a type
suffix:

| Function | Signature | Description |
|---|---|---|
| `absInt` | `int -> int` | Absolute value; traps on the minimum representable integer |
| `minInt` | `(int, int) -> int` | Smaller integer |
| `maxInt` | `(int, int) -> int` | Larger integer |
| `clampInt` | `(int, int, int) -> int` | Clamp value to inclusive lower/upper bounds; requires lower <= upper |
| `signInt` | `int -> int` | Returns -1, 0, or 1 |
| `isEven` | `int -> bool` | True when the integer is divisible by two |
| `isOdd` | `int -> bool` | True when the integer is not divisible by two |
| `absFloat` | `float -> float` | Absolute floating-point value |
| `minFloat` | `(float, float) -> float` | Smaller float |
| `maxFloat` | `(float, float) -> float` | Larger float |
| `absDouble` | `double -> double` | Absolute double-precision value |
| `minDouble` | `(double, double) -> double` | Smaller double |
| `maxDouble` | `(double, double) -> double` | Larger double |

These helpers are ordinary Serenity functions. They are expanded by the
preprocessor and compiled by the existing frontend, bytecode interpreter,
tracing JIT, and AOT backend. No backend-specific intrinsic is introduced by
this library.

## Status

This document describes the initial math-only v0.11 library. Serenity v0.12
adds primitive stream I/O and the `std/io.snh` wrappers. See
`docs/STANDARD_IO_v0.12.md` for the current I/O API.
