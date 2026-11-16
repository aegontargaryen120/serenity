# Serenity Lexer - milestone 0.1

This is the first implementation milestone for the Serenity language: a C++20 lexer and a small test executable.

## Build

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

No LLVM dependency is needed for the lexer milestone.

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
- `%` by itself is reserved for preprocessor directives. The preprocessor will run before the lexer and remove `%include` directives.

The lexer preserves each token's source location and reports recoverable lexical diagnostics. String tokens also carry a decoded string value.

## Scope note

This milestone implements tokenization only. It does not yet implement the `%include` preprocessor, parser, semantic analysis, bytecode, interpreter, AOT compiler, or tracing JIT.
