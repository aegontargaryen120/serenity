# Serenity - compiler frontend and bytecode generator v0.6

Serenity is a statically typed compiled language with an interpreter and a
planned tracing JIT. This repository currently contains the frontend through
semantic analysis plus the first bytecode generator.

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

The bytecode layer does not link LLVM. `sninterp` is planned to execute this
bytecode directly; `snjit` will later profile and compile hot code to native
machine code. The `serenity` AOT executable and LLVM backend are also future
milestones.

## Build and test

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

On macOS, CMake prefers the Homebrew LLVM toolchain when it exists:

```cmake
set(CMAKE_C_COMPILER "/opt/homebrew/opt/llvm/bin/clang")
set(CMAKE_CXX_COMPILER "/opt/homebrew/opt/llvm/bin/clang++")
```

On other systems, or where that Homebrew prefix does not exist, CMake uses the
system's normal compiler discovery.

## Bytecode generation API

Run the lexer, parser, and semantic analyzer first. Generate bytecode only when
both lexing/parsing and semantic analysis succeed:

```cpp
#include "serenity/bytecode/generator.hpp"
#include "serenity/lexer/lexer.hpp"
#include "serenity/parser/parser.hpp"
#include "serenity/semantic/analyzer.hpp"

#include <utility>

serenity::Lexer lexer(source);
auto lexed = lexer.tokenize();
if (!lexed.ok()) {
    // Report lexed.errors.
}

serenity::parser::Parser parser(std::move(lexed.tokens));
auto parsed = parser.parse();
if (!parsed.ok()) {
    // Report parsed.errors.
}

serenity::semantic::SemanticAnalyzer analyzer(*parsed.program);
auto checked = analyzer.analyze();
if (!checked.ok()) {
    // Report checked.errors.
}

serenity::bytecode::BytecodeGenerator generator(*parsed.program, checked.model);
auto generated = generator.generate();
if (generated.ok()) {
    const std::string listing = serenity::bytecode::disassemble(*generated.module);
}
```

The AST must remain alive and unchanged while its `SemanticModel` is used.

See `docs/BYTECODE_v0.6.md` for instruction layout, stack effects, function
calling convention, defaults, and current limitations. Earlier milestone notes
remain in `docs/`.
