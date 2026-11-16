#pragma once

#include "serenity/bytecode/module.hpp"

#include <optional>
#include <string>
#include <vector>

namespace serenity::aot {

struct LLVMError {
    std::string message;
    SourceLocation location {};
};

struct LLVMResult {
    std::optional<std::string> ir;
    std::vector<LLVMError> errors;

    [[nodiscard]] bool ok() const noexcept {
        return ir.has_value() && errors.empty();
    }
};

// Lowers Serenity's validated bytecode module into textual LLVM IR. The
// generated module uses native LLVM functions for Serenity functions and
// control flow, with a small C ABI runtime for tagged values and heap objects.
// This keeps the backend independent of LLVM's C++ library ABI.
class LLVMIREmitter {
public:
    explicit LLVMIREmitter(const bytecode::BytecodeModule& module);
    [[nodiscard]] LLVMResult emit() const;

private:
    const bytecode::BytecodeModule& module_;
};

} // namespace serenity::aot
