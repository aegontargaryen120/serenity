# Serenity AOT Compiler v0.10 — Direct LLVM API Backend

## Goal

Replace the v0.8 Clang-driver-based AOT pipeline with LLVM's C++ APIs for IR
parsing, verification, optimization, and native object-code generation.

The compiler no longer spawns `clang` or `clang++` during AOT compilation.
The LLVM 23 target-triple declarations are included from
`llvm/TargetParser/Triple.h`; the older `llvm/ADT/Triple.h` path is not present
in LLVM 23. Native assembly-parser initialization is unnecessary for object
emission and is intentionally omitted.

## Current pipeline

```text
Serenity source (.sn)
        |
   Preprocessor
        |
      Lexer
        |
      Parser
        |
 Semantic analysis
        |
 Bytecode generator
        |
 Serenity LLVM IR emitter (textual IR)
        |
 llvm::parseIR -> llvm::Module
        |
 LLVM verifier
        |
 PassBuilder optimization pipeline (-O1/-O2/-O3)
        |
 TargetMachine object-file emission
        |
 native object file (.o)
        |
 platform linker (/usr/bin/ld on macOS)
        |
 Serenity executable
```

## What uses LLVM directly?

`src/aot/llvm_emitter.cpp` remains the Serenity-owned bytecode-to-LLVM-IR
lowering stage. It emits LLVM IR text, which is useful for diagnostics and
`--emit-llvm` inspection.

`src/aot/llvm_backend.cpp` uses LLVM's C++ APIs to:

1. Parse the IR text with `llvm::parseIR` into an `llvm::Module`.
2. Select the host target with `TargetRegistry` and `TargetMachine`.
3. Set the module target triple and data layout.
4. Verify the module before optimization.
5. Run LLVM's new Pass Manager using `PassBuilder` for `-O1`, `-O2`, or `-O3`.
6. Verify the optimized module.
7. Emit a native object file using `TargetMachine::addPassesToEmitFile`.

`-O0` skips the IR optimization pipeline and asks the target code generator for
unoptimized machine code. `-O1` through `-O3` select matching IR and machine
code optimization levels.

## Linking

LLVM's `TargetMachine` emits an object file; it does not perform the final
platform executable link. On macOS the compiler invokes `/usr/bin/ld` directly
with the detected macOS SDK, deployment target, object file, Serenity runtime
archive, `libc++`, and `libSystem`.

No shell is used for this invocation. LLVM's `sys::ExecuteAndWait` passes each
linker argument as a separate argument, so spaces and shell metacharacters in
paths are not interpreted by a shell.

Direct final-executable linking is currently implemented for macOS. On other
hosts, `--emit-object` can produce a native object file, but the final OS link
step reports that it is not yet supported.

## Command line

```sh
# Native executable
serenity main.sn -o main -O2
./main

# Textual LLVM IR, before LLVM's optimization pipeline
serenity main.sn --emit-llvm -o main.ll

# Native object file, without final linking
serenity main.sn --emit-object -o main.o
```

## LLVM/CMake integration

The project uses the Homebrew LLVM toolchain when available:

```cmake
set(CMAKE_C_COMPILER "/opt/homebrew/opt/llvm/bin/clang")
set(CMAKE_CXX_COMPILER "/opt/homebrew/opt/llvm/bin/clang++")
```

Configure on Apple Silicon with:

```sh
cmake -S . -B build \
  -DLLVM_DIR="$(brew --prefix llvm)/lib/cmake/llvm" \
  -DSERENITY_REQUIRE_LLVM=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

CMake discovers the macOS SDK path and version via `xcrun`. The minimum
macOS deployment version is configurable through
`SERENITY_MACOS_MIN_VERSION` and defaults to `13.0`.

`sninterp` is not linked to LLVM. A build without the LLVM development package
can still build the frontend and interpreter, but `serenity` cannot emit native
objects and `snjit` is unavailable.

## Testing

- `serenity_aot_tests` checks the Serenity-owned bytecode-to-textual-IR emitter.
- `serenity_aot_backend_tests` (LLVM-enabled builds) checks direct parsing and
  native object emission through LLVM's C++ APIs.
- `serenity_aot_smoke` (LLVM-enabled macOS builds) compiles and links the sample
  program, runs it, and compares output with `sninterp`.
