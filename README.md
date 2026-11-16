# Serenity - compiler frontend, interpreter, and AOT compiler v0.8

Serenity is a statically typed compiled language with a shared frontend, a
bytecode interpreter, and an initial LLVM-based ahead-of-time compiler. A
tracing JIT (`snjit`) is planned to use the same bytecode format.

## Current components

- **Lexer v0.1:** tokenization and lexical diagnostics.
- **Preprocessor v0.2:** `%include` expansion, relative path resolution,
  include-once behavior, cycle handling, and source-origin mapping.
- **AST v0.3:** syntax tree model with source spans.
- **Parser v0.4:** recursive-descent parser and precedence-aware expressions.
- **Semantic analysis v0.5:** name/type resolution, signature checking,
  mutability and control-flow context checks.
- **Bytecode generator v0.6:** fixed-width stack instructions, constants,
  global/local slots, function calls, control flow, structs/enums, source spans,
  and disassembly.
- **Interpreter v0.7 (`sninterp`):** independent bytecode execution with call
  frames, runtime diagnostics, and execution statistics. It does not link LLVM.
- **AOT compiler v0.8 (`serenity`):** lowers bytecode into textual LLVM IR and
  invokes `clang++` to produce native executables.

The future **`snjit`** executable will interpret the same bytecode, profile hot
code, and compile traces to native machine code. Bytecode serialization (`.sbc`)
is not implemented yet.

## Build and test

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

On Apple Silicon macOS, CMake prefers the Homebrew LLVM toolchain:

```cmake
set(CMAKE_C_COMPILER "/opt/homebrew/opt/llvm/bin/clang")
set(CMAKE_CXX_COMPILER "/opt/homebrew/opt/llvm/bin/clang++")
```

The compiler records the discovered `clang++` executable and uses it to lower
LLVM IR and link the static Serenity runtime. LLVM is not linked into `sninterp`.

## Run a Serenity program

Interpret bytecode:

```sh
./build/sninterp examples/interpreter_demo.sn
```

Compile ahead of time:

```sh
./build/serenity examples/aot_demo.sn -o build/aot_demo -O2
./build/aot_demo
```

Emit LLVM IR for inspection:

```sh
./build/serenity examples/aot_demo.sn --emit-llvm -o build/aot_demo.ll
```

The AOT target defaults to the host selected by `clang++`; Homebrew LLVM on
Apple Silicon therefore generates arm64 macOS code by default.

## AOT backend architecture

The AOT compiler uses the same source pipeline as the interpreter:

1. `%include` preprocessing;
2. lexing;
3. parsing;
4. semantic analysis;
5. bytecode generation;
6. bytecode-to-LLVM-IR lowering;
7. Clang/LLVM optimization, native code generation, and linking.

The v0.8 LLVM emitter produces native functions, basic blocks, branches, and
direct calls. A small C ABI runtime currently handles tagged value operations,
printing, and struct object access. This prioritizes correctness and parity with
`sninterp`; lowering primitive operations directly into LLVM instructions is a
future optimization.

See `docs/AOT_v0.8.md` for the LLVM IR layout, runtime ABI, CLI, architecture,
and current limitations. Earlier milestone documents remain in `docs/`.
