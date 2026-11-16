# Serenity Frontend - milestone 0.4

This project contains the current Serenity frontend foundations:

- **Lexer v0.1:** tokenizes Serenity source and reports lexical diagnostics.
- **Preprocessor v0.2:** expands `%include` directives and preserves source origins.
- **AST v0.3:** defines the syntax tree data model.
- **Parser v0.4:** constructs ASTs from tokens using recursive descent and
  precedence-climbing expression parsing.

The project uses C++20 and does not depend on LLVM yet.

## Build and test

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The CMake configuration selects Homebrew LLVM's `clang` and `clang++` drivers.
The C++ compiler must be `clang++`, because it links the C++ standard library
and runtime.

## Parser usage

```cpp
#include "serenity/lexer/lexer.hpp"
#include "serenity/parser/parser.hpp"

#include <utility>

serenity::Lexer lexer(source);
auto lexed = lexer.tokenize();

if (lexed.ok()) {
    serenity::parser::Parser parser(std::move(lexed.tokens));
    auto parsed = parser.parse();
    if (parsed.ok()) {
        // Use parsed.program.
    } else {
        // Report parsed.errors.
    }
}
```

The parser owns its token vector. `ParseResult` contains a `Program` and any
syntax errors. It can retain a partial AST after recoverable errors.

## Source locations

Every AST node carries a half-open source span. Offsets refer to the
preprocessed source buffer. Use `PreprocessResult::originAt(offset)` to map an
AST span or parser error back to the original `.sn` or `.snh` file.

## Preprocessor

Serenity uses `%include` directives. Included `.snh` files are expanded before
lexing, relative to the file containing the directive. Duplicate and circular
includes are handled automatically.

## Current project layout

```text
include/serenity/lexer/         Lexer and token interfaces
include/serenity/preprocessor/  Preprocessor interface
include/serenity/ast/           AST node definitions
include/serenity/parser/        Parser interface
src/lexer/                      Lexer implementation
src/preprocessor/               Preprocessor implementation
src/parser/                     Parser implementation
tests/                          Lexer, preprocessor, AST, and parser tests
docs/                            Grammar and design notes
```

## Parser scope and limitations

The parser supports top-level global variables, functions, structs, and enums.
Executable statements must be inside functions. It supports local variables,
assignments, function calls, returns, `if`/`else`, `while`, `break`, `continue`,
blocks, and the currently specified expression operators.

`for` is reserved but not implemented. `return;`, arrays, indexing, a `void`
type, module declarations, function overloading, and semantic checks are not
implemented in this milestone. See `docs/PARSER_DESIGN.md` and
`docs/GRAMMAR_v0.1.md` for details.
