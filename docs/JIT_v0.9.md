# Serenity Tracing JIT v0.9

## Goal

`snjit` runs the same Serenity bytecode as `sninterp`. It begins by executing
bytecode in the existing interpreter, counts backward branches, records a hot
loop's executed path, lowers supported traces to LLVM IR, and asks LLVM ORC
(LLJIT) to compile that IR into native code in the running process.

This is a tracing JIT, not a function-at-a-time eager compiler. A trace records
one observed path through a loop. Conditional branches include guards; when a
later iteration takes a different path, the compiled trace returns to the
interpreter at the alternate bytecode PC. Arithmetic checks similarly side-exit
before the operation so the interpreter preserves its runtime error behavior.

## Components

- **Profiler:** counts executed backward `JUMP` edges by function and target PC.
- **Trace recorder:** after a loop crosses the hot threshold, records one
  backedge-to-backedge path.
- **Trace verifier/lowerer:** checks path continuity, stack-depth agreement,
  slot types, and supported opcodes before generating LLVM IR.
- **ORC backend:** parses the generated IR in-process, adds it to `LLJIT`, and
  looks up the native trace entry point.
- **Interpreter integration:** calls compiled traces at loop headers, restores
  deoptimization stack state, and resumes bytecode at side exits.

## Initial trace support

The first trace compiler specializes `int` and `bool` local/global slots. It
supports:

- integer and boolean constants;
- local/global loads and stores;
- `DUP`, `POP`, `NOP`;
- checked integer `+`, `-`, `*`, `/`, `%%`, and unary negation;
- integer and boolean comparisons;
- logical NOT and bitwise NOT;
- bitwise AND, OR, XOR, and checked shifts;
- `JUMP`, `JUMP_IF_FALSE`, and loop backedges.

Calls, printing, strings, floating point, structs, enums, and other unsupported
operations are intentionally not traced in this first milestone. Such loops
continue in `sninterp`; they are not incorrectly partially compiled.

A trace is compiled only when its recorded path is continuous, the operand
stack is empty at the loop header/backedge, every stack depth matches the
interpreter's observations, and all accessed slots are `int` or `bool`.

## Deoptimization and safety

Compiled traces receive a finite instruction budget from the interpreter. They
check it before every bytecode operation, so an infinite loop cannot bypass the
interpreter's `maxInstructions` safety ceiling.

The trace lowers potentially failing integer operations with guards:

- integer add/subtract/multiply overflow;
- division or remainder by zero;
- signed minimum divided/remaindered by -1;
- invalid/overflowing shifts;
- negation of the minimum signed integer.

When one of these guards fails, the trace materializes its operand stack and
returns to the interpreter at the original instruction. The interpreter then
produces the same runtime diagnostic it would have produced without JIT.

A conditional trace guard exits after consuming its boolean condition and sets
the interpreter PC to the alternate branch target. Local/global mutations made
by earlier trace operations are copied back before interpretation resumes.

## Build

On Apple Silicon with Homebrew LLVM:

```sh
brew install llvm
cmake -S . -B build \
  -DLLVM_DIR="$(brew --prefix llvm)/lib/cmake/llvm" \
  -DSERENITY_REQUIRE_LLVM=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

CMake searches `/opt/homebrew/opt/llvm` automatically. With
`SERENITY_REQUIRE_LLVM=ON`, a missing LLVM development package stops
configuration instead of silently producing an unavailable `snjit` stub.
`sninterp` and the rest of Serenity remain independent of LLVM.

## Usage

```sh
./build/snjit examples/jit_demo.sn
./build/snjit examples/jit_demo.sn --hot-threshold 4 --stats
```

Options:

- `--hot-threshold N`: number of backedges before recording begins (default 8).
- `--max-instructions N`: execution safety ceiling (default 100,000,000; zero
  disables it).
- `--stats`: print profiling, compilation, trace execution, side-exit, and
  instruction-count statistics to stderr.

The same source can be run through `sninterp` for a correctness/performance
comparison:

```sh
./build/sninterp examples/jit_demo.sn
./build/snjit examples/jit_demo.sn --stats
```

For meaningful performance measurements, compare repeated runs, separate cold
JIT startup/compilation cost from warmed native execution, and use a workload
large enough to amortize profiling and trace compilation.

## Current limitations

- Only hot loops with the supported integer/boolean bytecode subset are
  compiled. Other code remains interpreted.
- The first implementation records a single observed path per loop; changing
  branch paths side-exit rather than compiling additional side traces.
- Trace-local/global values are currently copied through an integer scratch
  area at trace entry/exit. A future VM frame layout can avoid this copy.
- No on-disk bytecode serialization or cross-run trace cache exists.
- This is a first tracing JIT, not PyPy's meta-tracing architecture. PyPy traces
  the interpreter (meta-tracing); Serenity currently traces the running
  Serenity bytecode program.
