# Serenity v0.9 — tracing JIT

Serenity is a statically typed compiled language with a shared frontend, a
bytecode interpreter, an AOT compiler, and an initial tracing JIT.

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
  invokes the installed `clang++` driver to produce native executables.
- **Tracing JIT v0.9.2 (`snjit`):** runs the same bytecode in the interpreter,
  profiles backward loop edges, records hot execution paths, lowers supported
  integer/boolean traces to LLVM IR, and compiles them in-process through LLVM
  ORC LLJIT. Unsupported loops remain interpreted.

Bytecode serialization (`.sbc`) is not implemented yet.

## Build and test

On Apple Silicon with Homebrew LLVM installed:

```sh
cmake -S . -B build \
  -DLLVM_DIR="$(brew --prefix llvm)/lib/cmake/llvm" \
  -DSERENITY_REQUIRE_LLVM=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

CMake prefers the user's Homebrew compiler drivers when available:

```cmake
set(CMAKE_C_COMPILER "/opt/homebrew/opt/llvm/bin/clang")
set(CMAKE_CXX_COMPILER "/opt/homebrew/opt/llvm/bin/clang++")
```

CMake also searches `/opt/homebrew/opt/llvm` for `LLVMConfig.cmake`. The
explicit `LLVM_DIR` argument above removes any ambiguity. A successful CMake
configuration should print a message similar to:

```text
Serenity tracing JIT: LLVM <version> found at <...>/lib/cmake/llvm
```

If CMake reports that the LLVM development package was not found, `snjit` is
built as an explicit unavailable stub. The other tools still build, and
`sninterp` remains entirely independent of LLVM. Check that Homebrew's LLVM
package contains `include/llvm` and `lib/cmake/llvm/LLVMConfig.cmake`, then
reconfigure with the `LLVM_DIR` command above.


## LLVM 23.1.x compatibility update (v0.9.2)

The ORC backend includes `llvm/IRReader/IRReader.h` for the C++ `llvm::parseIR`
API, and passes `LLJIT::getTargetTriple()` directly to
`Module::setTargetTriple()`. LLVM 23 exposes that target triple as an
`llvm::Triple` object; converting it to a string is not accepted by the
current `Module` API. The JIT links the `IRReader` component in the
non-monolithic LLVM-library configuration.

After updating, remove the old CMake build directory and configure again.
Only run CTest after `cmake --build` completes successfully, because a
failed build can leave registered test executables missing.

## Run a Serenity program

Interpret bytecode:

```sh
./build/sninterp examples/jit_demo.sn
./build/sninterp examples/jit_demo.sn --stats
```

Run with the tracing JIT:

```sh
./build/snjit examples/jit_demo.sn
./build/snjit examples/jit_demo.sn --hot-threshold 4 --stats
```

Compile ahead of time:

```sh
./build/serenity examples/aot_demo.sn -o build/aot_demo -O2
./build/aot_demo
```

Emit LLVM IR from the AOT compiler:

```sh
./build/serenity examples/aot_demo.sn --emit-llvm -o build/aot_demo.ll
```

## Tracing JIT architecture

`snjit` and `sninterp` use the same front-end and bytecode representation.
The JIT execution pipeline is:

1. The ordinary bytecode interpreter begins executing the program.
2. A profiler counts backward `JUMP` edges by function and loop-header PC.
3. Once a loop crosses the hot threshold, the runtime records one observed
   path from its header back to its backedge.
4. The trace verifier checks path continuity, stack depth, slot types, and
   opcode support.
5. The trace lowerer emits LLVM IR with guards and deoptimization exits.
6. LLVM ORC LLJIT compiles the trace in the running process.
7. The interpreter enters the native trace at the loop header. A changed branch
   or failed arithmetic guard materializes VM state and resumes interpretation.

LLVM's ORC `LLJIT` provides in-process LLVM IR compilation and symbol lookup;
Serenity supplies the profiler, recorder, trace lowering, guards, and
interpreter integration. See the [LLVM ORC documentation](https://llvm.org/docs/ORCv2.html).

## Initial trace support

The first trace compiler specializes `int` and `bool` local/global slots. It
supports:

- integer and boolean constants;
- local/global loads and stores;
- `DUP`, `POP`, and `NOP`;
- checked integer `+`, `-`, `*`, `/`, `%%`, and unary negation;
- integer and boolean comparisons;
- logical NOT and bitwise NOT;
- bitwise AND, OR, XOR, and checked shifts;
- `JUMP`, `JUMP_IF_FALSE`, and loop backedges.

Calls, printing, strings, floating point, structs, enums, and other unsupported
operations are not compiled into traces. They continue to execute in the
interpreter. A loop containing unsupported operations is rejected as a whole
rather than partially compiled.

The first implementation records one path per loop. A changed conditional
branch side-exits to the interpreter; side traces and multi-path trace trees
are future work. Trace entry/exit copies touched integer and boolean slots via
a scratch buffer. A future VM frame layout could avoid those copies.

## Safety and deoptimization

Compiled traces receive the remaining interpreter instruction budget and check
it before every bytecode operation. This prevents a native infinite loop from
bypassing the interpreter's instruction limit.

Potentially failing integer operations use guards. Overflow, division/remainder
by zero, signed-minimum divided by -1, invalid shifts, and negation of the
signed minimum deopt before executing the operation. The complete operand stack
is restored, including any prefix values below the operation's operands, and
the interpreter executes the original instruction to preserve its normal
runtime diagnostic.

Conditional guards resume at the alternate bytecode successor after consuming
the condition, and mutations made by previously executed trace operations are
copied back to the VM slots.

## Statistics and comparison

Both executables support `--stats`:

```sh
./build/sninterp examples/jit_demo.sn --stats
./build/snjit examples/jit_demo.sn --stats
```

JIT statistics include observed backedges, recording attempts, compiled and
rejected traces, trace executions, native bytecode instruction counts, loop
iterations, side exits, budget exits, deoptimizations, and compilation time.

For meaningful comparisons, measure cold startup and warmed execution
separately, use repeated runs, and select a workload large enough to amortize
profiling and compilation overhead.

## Current limitations

- Only hot loops using the listed integer/boolean bytecode subset are compiled.
- One observed path is recorded per loop; changed branch paths side-exit rather
  than generating side traces.
- Calls and output operations inside a loop currently prevent that loop from
  being compiled as a trace.
- There is no on-disk bytecode serialization or cross-run trace cache.
- This is a bytecode tracing JIT, not PyPy's meta-tracing architecture. PyPy
  traces its interpreter; Serenity currently traces the running Serenity
  bytecode program.
- The current build/test environment used for this release did not include
  LLVM development headers/libraries. The LLVM-disabled build, all non-ORC
  targets, and the fallback test path were verified here. The ORC-enabled
  branch must be compiled and exercised on a machine with the LLVM development
  package installed; this project's Apple Silicon setup is documented above.

See `docs/JIT_v0.9.md` for the detailed trace format, bytecode hook contract,
LLVM lowering, statistics, and limitations. Earlier milestone documents remain
in `docs/`.
