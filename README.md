# Serenity Frontend - milestone 0.2

This project contains the first two Serenity frontend components:

- **Lexer v0.1:** tokenizes Serenity source and reports lexical diagnostics.
- **Preprocessor v0.2:** expands `%include` directives before lexing and preserves a source map back to the original `.sn` and `.snh` files.

The implementation uses C++20 and has no LLVM dependency.

## Build and test

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Preprocessor syntax

Serenity uses `%include`, not `#include`:

```serenity
%include "math.snh"
%include "io.snh"
```

Single-quoted paths are also accepted:

```serenity
%include 'math.snh'
```

A directive must be the first non-whitespace content on its physical line, and it must appear outside strings and comments. An optional trailing `//` comment is permitted:

```serenity
    %include "math.snh" // include declarations from math
```

Only `.snh` files can be included. Paths are resolved relative to the file containing the directive. Command-line include directories are not implemented yet.

## Automatic header guards

Every `.snh` file is included at most once during a preprocessing run. The preprocessor identifies files using normalized canonical paths, so including the same header through different relative path spellings does not expand it twice.

Circular includes are stopped by the same automatic guard. A circular include produces a warning and preprocessing continues. Missing files, malformed directives, unsupported extensions, and unknown directives produce errors.

## Source locations

The preprocessor returns:

- `source`: the expanded source text passed to the lexer.
- `sourceMap`: ranges mapping expanded-source offsets to original files and source positions.
- `diagnostics`: preprocessing warnings and errors.

Use `PreprocessResult::originAt(offset)` to map an offset in the expanded text back to its original filename, line, and column. The map is stored as ranges rather than one entry per character.

Example integration:

```cpp
#include "serenity/preprocessor/preprocessor.hpp"
#include "serenity/lexer/lexer.hpp"

serenity::Preprocessor preprocessor;
auto preprocessed = preprocessor.preprocessFile("src/main.sn");

if (!preprocessed.ok()) {
    // Report preprocessed.diagnostics.
    return 1;
}

serenity::Lexer lexer(std::move(preprocessed.source));
auto lexed = lexer.tokenize();

// For a token at token.location.offset, use preprocessed.originAt(...)
// to recover its original .sn or .snh location.
```

The current `Lexer` still reports offsets and line/column positions in the expanded text. The preprocessor source map is the bridge that lets the compiler driver translate those positions back to their original files. A later integration step can make the diagnostic layer do this automatically.

## Current lexical rules

- ASCII identifiers: `[A-Za-z_][A-Za-z0-9_]*`
- Keywords: `mut`, `func`, `return`, `if`, `else`, `while`, `for`, `break`, `continue`, `struct`, `enum`, `int`, `float`, `double`, `bool`, `string`, `true`, `false`
- Decimal integer and decimal floating-point literals only.
- Both single-quoted and double-quoted strings.
- Escape sequences: `\\a`, `\\b`, `\\f`, `\\n`, `\\r`, `\\t`, `\\v`, `\\\\`, `\\'`, `\\"`, `\\0`.
- Backslash-newline inside a string removes the physical newline.
- A backslash-newline between adjacent string literals joins their decoded values.
- `//` line comments and `/* ... */` block comments.
- Remainder operator: `%%`; remainder assignment: `%%=`.
- Not-equal operator: `=/=`. The spelling `!=` is diagnosed.
- A bare `%` is reserved for preprocessor directives and is not a remainder operator.

## Project layout

```text
include/serenity/lexer/       Lexer and token interfaces
include/serenity/preprocessor/ Preprocessor interface
src/lexer/                    Lexer implementation
src/preprocessor/             Preprocessor implementation
tests/                        Lexer and preprocessor tests
```

## Not implemented yet

The parser, AST, semantic analysis, bytecode generator, interpreter, AOT backend, runtime, garbage collector, and tracing JIT are future milestones.
