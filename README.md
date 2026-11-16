# Serenity Frontend - milestone 0.3

This project contains the current Serenity frontend foundations:

- **Lexer v0.1:** tokenizes Serenity source and reports lexical diagnostics.
- **Preprocessor v0.2:** expands `%include` directives and preserves source origins.
- **AST v0.3:** defines the syntax tree data model for programs, declarations,
  statements, expressions, and source spans.

The project uses C++20 and has no LLVM dependency yet.

## Build and test

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The CMake configuration selects Homebrew LLVM's `clang` and `clang++` drivers.

## AST status

The AST is currently a data model, not a parser. It deliberately stores syntax
and source spans without doing semantic analysis. See [docs/AST_DESIGN.md](docs/AST_DESIGN.md)
for the node model and design notes.

`null` is now a reserved keyword and is tokenized as `KwNull`.

## Preprocessor

Serenity uses `%include` directives. Included `.snh` files are expanded before
lexing, relative to the file containing the directive. Duplicate and circular
includes are handled automatically. `PreprocessResult::originAt(offset)` maps
expanded-source offsets back to original source locations.

## Current project layout

```text
include/serenity/lexer/         Lexer and token interfaces
include/serenity/preprocessor/  Preprocessor interface
include/serenity/ast/           AST node definitions
src/lexer/                      Lexer implementation
src/preprocessor/               Preprocessor implementation
tests/                          Lexer, preprocessor, and AST tests
docs/                            Design notes
```
