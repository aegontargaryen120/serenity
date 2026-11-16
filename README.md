# Serenity Frontend - milestone 0.5

This project contains the current Serenity frontend foundations:

- **Lexer v0.1:** tokenizes Serenity source and reports lexical diagnostics.
- **Preprocessor v0.2:** expands `%include` directives and preserves source origins.
- **AST v0.3:** defines the syntax tree data model.
- **Parser v0.4:** constructs ASTs from tokens using recursive descent and
  precedence-climbing expression parsing.
- **Semantic Analyzer v0.5:** resolves names and types, validates declarations,
  checks function signatures and calls, enforces mutability, and validates
  return/loop contexts.

The project uses C++20. LLVM is not linked yet; it will be introduced with the
AOT/JIT backend milestones.

## Build and test

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The CMake configuration selects Homebrew LLVM's `clang` and `clang++` drivers.
The C++ compiler must be `clang++`, because it links the C++ standard library
and runtime.

## Semantic analyzer usage

```cpp
#include "serenity/lexer/lexer.hpp"
#include "serenity/parser/parser.hpp"
#include "serenity/semantic/analyzer.hpp"

#include <utility>

serenity::Lexer lexer(source);
auto lexed = lexer.tokenize();

if (lexed.ok()) {
    serenity::parser::Parser parser(std::move(lexed.tokens));
    auto parsed = parser.parse();

    if (parsed.ok()) {
        serenity::semantic::SemanticAnalyzer analyzer(*parsed.program);
        auto checked = analyzer.analyze();
        if (checked.ok()) {
            // checked.model.expressionTypes contains resolved expression types.
        } else {
            // Report checked.errors.
        }
    }
}
```

`SemanticAnalyzer` does not modify the AST. The returned semantic model stores
expression types keyed by pointers into the AST. Keep the `Program` alive and
unmodified while using that model.

## Source locations

Every AST node carries a half-open source span. Offsets refer to the
preprocessed source buffer. Use `PreprocessResult::originAt(offset)` to map an
AST span or diagnostic back to the original `.sn` or `.snh` file.

## Current project layout

```text
include/serenity/lexer/         Lexer and token interfaces
include/serenity/preprocessor/  Preprocessor interface
include/serenity/ast/           AST node definitions
include/serenity/parser/        Parser interface
include/serenity/semantic/      Semantic analyzer interface
src/lexer/                      Lexer implementation
src/preprocessor/               Preprocessor implementation
src/parser/                     Parser implementation
src/semantic/                   Semantic analyzer implementation
tests/                          Lexer, preprocessor, AST, parser, semantic tests
docs/                            Grammar and design notes
```

See `docs/SEMANTIC_ANALYSIS_v0.5.md` for the currently implemented rules and
intentional limitations.
