# Serenity Bytecode v0.7

## Purpose

The bytecode layer is the shared execution representation for `sninterp` and
`snjit`. It is deliberately independent of LLVM. The AOT compiler may also
consume the same bytecode or a later lower-level IR, but LLVM is not linked by
this milestone.

The v0.7 deliverable provides:

- a fixed-width instruction structure;
- typed constants and nominal type descriptors;
- global and local slot tables;
- function descriptors and function indices;
- bytecode generation from a semantically checked AST;
- source-span metadata for every generated instruction;
- a human-readable disassembler.

It does not yet execute or serialize bytecode. Execution belongs to `sninterp`
(v0.7); profiling and tracing compilation belong to `snjit` (later).

## Instruction representation

Every instruction has the following 12-byte logical layout:

| Field | Width | Purpose |
|---|---:|---|
| `opcode` | 8 bits | Operation |
| `type` | 8 bits | Type used by typed operations |
| `flags` | 16 bits | Reserved for future use; currently zero |
| `operand` | 32 bits | Constant, slot, function, descriptor, or jump index |
| `extra` | 32 bits | Secondary index or argument count |

Indices and jump targets are zero-based. Jump targets refer to instruction
indices inside the current function. The C++ structure is checked with a
`static_assert(sizeof(Instruction) == 12)`.

The in-memory structure must **not** be written to disk with a raw `write()` of
its bytes. A future `.sbc` writer must serialize each field explicitly in
little-endian order so padding and host endianness cannot change the format.

## Execution model

Serenity bytecode is stack-based. Expressions leave their result on the operand
stack. Statements generally consume their results.

Important stack effects:

| Instruction | Stack effect |
|---|---|
| `PUSH_CONST` | `... -> ..., value` |
| `PUSH_ENUM` | `... -> ..., enum-value` |
| `LOAD_LOCAL` / `LOAD_GLOBAL` | `... -> ..., value` |
| `STORE_LOCAL` / `STORE_GLOBAL` | `..., value -> ...` |
| `DUP` | `..., value -> ..., value, value` |
| `POP` | `..., value -> ...` |
| `NEW_STRUCT` | `... -> ..., struct-reference` |
| `GET_FIELD` | `..., struct-reference -> ..., field-value` |
| `SET_FIELD` | `..., struct-reference, value -> ...` |
| arithmetic/comparison | `..., left, right -> ..., result` |
| `JUMP_IF_FALSE` | `..., bool -> ...` |
| `CALL` | `..., arg0, ..., argN -> ..., return-value` |
| `PRINT` / `PRINTLN` | `..., printable-value -> ...` |
| `RETURN` | consumes the function's return value (except synthetic void initializer) |

Arguments are evaluated left-to-right. A `CALL` instruction stores the target
function index in `operand` and argument count in `extra`. The callee's parameter
values occupy its first local slots in declaration order.

`JUMP_IF_FALSE` consumes its boolean condition. `&&` and `||` are emitted as
conditional branches, not eager binary operations, so they short-circuit.

## Functions and slots

Each function owns its instruction vector and local-slot metadata. Function
indices are stable within one `BytecodeModule`. Local slots are allocated in
declaration order; scopes resolve names during generation, but slots are not
reused when a lexical scope ends. Parameters occupy slots `0..parameterCount-1`.

Global variables have a separate module-wide slot table. A synthetic function
named `__serenity_init_globals` initializes every global to its language default
first, then evaluates explicit initializers in source order. This ensures all
globals have a defined default before any explicit global initializer runs.

If a function named `main` has a definition, its index is recorded as the
module's optional entry function. The generator does not yet enforce a required
`main` signature; that belongs to the command-line/runtime contract.

## Types and values

`int` values use signed 64-bit storage in the initial VM representation.
`float` and `double` remain distinct. `bool`, `string`, `null`, struct, and enum
values have distinct bytecode type tags. Struct and enum descriptors retain
their nominal names.

Default values emitted for uninitialized variables are:

| Type | Bytecode value |
|---|---|
| `int` | integer zero |
| `float` | floating zero |
| `double` | double zero |
| `bool` | `false` |
| `string` | `null` |
| Struct | `null` |
| Enum | `null` |

Struct descriptors list fields in declaration order. A field may refer to a
constant-pool entry for its declared default. `NEW_STRUCT` is expected to create
a value initialized from those field defaults, falling back to the type default
for omitted defaults. Designated initializers then overwrite the supplied fields
using `DUP` and `SET_FIELD`.

In v0.7, struct field defaults must be compile-time constant expressions. The
constant evaluator handles primitive literals, supported unary operations, and
basic constant binary operations. A default that requires a function call or a
runtime variable is diagnosed by the bytecode generator.

Enum values are emitted with `PUSH_ENUM`: `operand` is the enum descriptor
index and `extra` is the zero-based member ordinal.

## Built-ins

The current `print(value)` and `println(value)` built-ins are emitted as
`PRINT` and `PRINTLN`. They consume one value. The type field identifies the
printable primitive type. They do not produce a result.

## Source locations

Every generated instruction has a matching entry in the function's
`debugLocations` vector. These spans refer to the preprocessed source buffer.
The preprocessor's origin map can translate the span's starting offset to the
original `.sn` or `.snh` file for runtime diagnostics and debugging.

## Current limitations

- No bytecode interpreter yet; that is v0.7 (`sninterp`).
- No tracing profiler/JIT yet; that is the later `snjit` milestone.
- No `.sbc` binary serialization or loader yet.
- No verifier for externally loaded/untrusted bytecode yet.
- Function prototypes without definitions are rejected because v0.7 has no
  external linking contract.
- `for`, arrays, and indexing are not part of the current parser/AST.
- A function without an explicit return on every path receives a generated
  type-default fallback return. Definite-return analysis is still future work.
- Struct field defaults that are not compile-time constants are unsupported.


## v0.7 additions

Bytecode v0.7 adds type-erased `any` values, stream handles, and the
`PUSH_STREAM`, `STREAM_READ`, and `STREAM_WRITE` instructions used by the
standard I/O primitives. `STREAM_READ` consumes a stream handle and produces
a nullable string. `STREAM_WRITE` consumes a stream handle and a printable
value. The existing bytecode instruction record remains 12 bytes wide.
