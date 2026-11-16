#pragma once

#include "serenity/ast/ast.hpp"
#include "serenity/bytecode/module.hpp"
#include "serenity/semantic/analyzer.hpp"

#include <optional>
#include <string>
#include <vector>

namespace serenity::bytecode {

struct BytecodeError {
    std::string message;
    SourceLocation location {};
};

struct BytecodeResult {
    std::optional<BytecodeModule> module;
    std::vector<BytecodeError> errors;

    [[nodiscard]] bool ok() const noexcept {
        return module.has_value() && errors.empty();
    }
};

// Generates stack bytecode from a semantically checked AST. The Program must
// remain alive and unchanged for the lifetime of the SemanticModel references.
class BytecodeGenerator {
public:
    BytecodeGenerator(const ast::Program& program, const semantic::SemanticModel& semanticModel);
    [[nodiscard]] BytecodeResult generate();

private:
    const ast::Program& program_;
    const semantic::SemanticModel& semanticModel_;
};

} // namespace serenity::bytecode
