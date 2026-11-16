# Serenity JIT v0.9.2 — LLVM 23 compatibility fix

This patch resolves two compile errors reported with Homebrew LLVM 23.1.5.

1. `llvm::parseIR` is declared by `llvm/IRReader/IRReader.h`. Including
   `llvm/AsmParser/Parser.h` alone does not declare it.
2. `llvm::orc::LLJIT::getTargetTriple()` returns `const llvm::Triple&`, and
   `llvm::Module::setTargetTriple()` accepts an `llvm::Triple`. Pass the Triple
   object directly rather than calling `.str()`.

The implementation now uses:

```cpp
#include <llvm/IRReader/IRReader.h>

// ...
module->setTargetTriple(llvmJit_->getTargetTriple());
```

The CMake fallback component list already includes `IRReader`, so no additional
link component is needed.

## Rebuild

```sh
cd ~/code/serenity
rm -rf build
cmake -S . -B build \
  -DLLVM_DIR="$(brew --prefix llvm)/lib/cmake/llvm" \
  -DSERENITY_REQUIRE_LLVM=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Run `ctest` only if the build command exits successfully. The local packaging
checks for this patch use the LLVM-disabled configuration because the build
environment does not contain LLVM development headers; they do not claim to
verify the ORC-enabled compilation. The two fixes above are grounded in the
LLVM 23 API declarations.
