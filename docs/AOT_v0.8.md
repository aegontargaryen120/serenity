# Serenity AOT Compiler v0.8

## Purpose

`serenity` compiles Serenity source files ahead of time into native executables.
The compiler runs the same source frontend and bytecode generator used by
`sninterp`. The AOT backend then lowers that bytecode module into textual LLVM
IR and invokes the installed `clang++` driver to optimize, generate native code,
and link the Serenity runtime.

This makes the bytecode module the shared execution representation for the
interpreter and the future tracing JIT, while allowing AOT compilation to use
the same validated instructions rather than maintaining a second AST lowering
pipeline.

## Build and toolchain

On Apple Silicon macOS, CMake prefers the user's Homebrew LLVM installation:

```cmake
set(CMAKE_C_COMPILER "/opt/homebrew/opt/llvm/bin/clang")
set(CMAKE_CXX_COMPILER "/opt/homebrew/opt/llvm/bin/clang++")
```

The build discovers `clang++` from Homebrew's LLVM prefix and records that
path in the `serenity` compiler executable. `serenity_runtime` is built as a
static library and linked into every generated native executable.

The backend uses **textual LLVM IR plus the Clang driver**, rather than linking
the compiler executable against LLVM's C++ libraries. Clang itself uses LLVM
to verify/optimize the IR and generate machine code. This avoids coupling
Serenity to LLVM's C++ library ABI and version-specific CMake package layout.

`sninterp` does not link LLVM and remains independently usable.

## Usage

Compile to a native executable:

```sh
./build/serenity examples/aot_demo.sn -o build/aot_demo
./build/aot_demo
```

Select an LLVM optimization level:

```sh
./build/serenity examples/aot_demo.sn -o build/aot_demo -O2
```

Emit LLVM IR without linking:

```sh
./build/serenity examples/aot_demo.sn --emit-llvm -o build/aot_demo.ll
```

Supported options are `-o`, `-O0`, `-O1`, `-O2`, `-O3`, `--emit-llvm`,
`--help`, and `--version`. The target defaults to the host target selected by
the configured `clang++` driver. Therefore an Apple Silicon installation
produces an arm64 macOS executable by default.

The initial entry-point contract is `func main() => int { ... }` (a parameterless
`main` returning `int`; an internal void entry is also tolerated by the backend).
The generated native wrapper calls global initialization first, invokes Serenity
`main`, and returns the low byte of its integer result as the process exit code.

## LLVM lowering model

Each bytecode function becomes a native LLVM function with this internal ABI:

```llvm
define internal void @sn_fn_N(ptr %args, ptr %out)
```

Serenity values cross this internal ABI in a 16-byte tagged representation:

```llvm
%SnValue = type { i64, i64 }
```

The first word is the bytecode `ValueType` tag; the second is the payload
(integer bits, IEEE float/double bits, string pointer, enum value, or struct
pointer). Local variables and the operand stack are represented by native
stack allocations. Bytecode jumps become LLVM basic-block branches, and
Serenity function calls become direct calls to the corresponding generated
LLVM functions.

A control-flow dataflow pass verifies stack depth at every instruction and
requires matching stack depths at control-flow merge points before emitting IR.

A small C ABI runtime handles tagged arithmetic/comparisons, printing,
struct-object allocation and field access, default initialization, and process
exit-code conversion. This is an intentionally conservative first backend:
function bodies, loops, branches, and calls are native code, while many typed
value operations still pass through runtime helpers. Later AOT optimization
work can lower primitive arithmetic and comparisons directly into LLVM
instructions and reduce helper-call overhead.

## Runtime and memory

`src/runtime/runtime.cpp` implements the C ABI. String literals are emitted as
read-only LLVM globals and passed to the runtime as stable pointers. Struct
instances are allocated in a process-lifetime arena and remain valid for the
program's duration. This is a temporary ownership strategy, not the final
tracing garbage collector. Cyclic references and reclamation are addressed in
a later runtime milestone.

The language's current default values are preserved:

- `int`: zero;
- `float` and `double`: positive zero;
- `bool`: false;
- `string`, struct and enum values: null.

## Diagnostics and safety

The frontend reports preprocessing, lexical, parsing, semantic, and bytecode
errors before native compilation. The AOT backend validates function indices,
call argument counts, control-flow targets, stack underflow, and stack-depth
merges while lowering bytecode.

Runtime helper failures (for example, integer division by zero, integer
arithmetic overflow, invalid shifts, null struct access, or printing null)
print a runtime error and terminate the process with a nonzero status.
Unlike `sninterp`, v0.8 runtime failures do not yet map back to Serenity source
file/line locations; that metadata wiring is a future improvement.

## Current limitations

- No `.sbc` serialization or loader; source is compiled in-process.
- No tracing JIT yet; `snjit` will continue from the same bytecode module.
- No tracing garbage collector; struct objects live until process exit.
- No string concatenation or mutable string operations.
- `print()` and `println()` accept one non-null primitive argument.
- Struct field defaults must be compile-time constants, as in the bytecode
  generator.
- AOT runtime errors do not yet include original `.sn`/`.snh` source locations.
- The compiler records the build-tree runtime archive path. A relocatable
  installation/package layout is a later packaging task.
- Optimization is delegated to the configured Clang/LLVM driver via `-O0` to
  `-O3`; custom Serenity-specific optimization passes are not implemented.
