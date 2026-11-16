#pragma once

#include "serenity/bytecode/instruction.hpp"
#include "serenity/ast/ast.hpp"
#include "serenity/lexer/token.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace serenity::bytecode {

struct TypeInfo {
    ValueType kind {ValueType::Invalid};
    // Populated for Struct and Enum types. Primitive types leave this empty.
    std::string nominalName;
};

struct EnumConstantValue {
    std::uint32_t enumIndex {0};
    std::uint32_t memberIndex {0};
};

using ConstantValue = std::variant<
    std::monostate, std::int64_t, float, double, bool, std::string, EnumConstantValue>;

struct Constant {
    ValueType type {ValueType::Invalid};
    ConstantValue value;
};

struct LocalSlot {
    std::string name;
    TypeInfo type;
    bool isMutable {false};
    bool isParameter {false};
};

struct GlobalSlot {
    std::string name;
    TypeInfo type;
    bool isMutable {false};
};

struct FieldDescriptor {
    std::string name;
    TypeInfo type;
    // Missing means use the type's language-defined default.
    std::optional<std::uint32_t> defaultConstant;
};

struct StructDescriptor {
    std::string name;
    std::vector<FieldDescriptor> fields;
};

struct EnumDescriptor {
    std::string name;
    std::vector<std::string> members;
};

struct Function {
    std::string name;
    std::vector<TypeInfo> parameterTypes;
    TypeInfo returnType;
    std::vector<LocalSlot> locals;
    std::vector<Instruction> code;
    // One source span per instruction. Spans refer to the preprocessed source;
    // the preprocessor source map can translate them to original files.
    std::vector<ast::SourceSpan> debugLocations;
    bool synthetic {false};
};

struct BytecodeModule {
    std::uint16_t versionMajor {0};
    std::uint16_t versionMinor {6};
    std::vector<Constant> constants;
    std::vector<GlobalSlot> globals;
    std::vector<StructDescriptor> structs;
    std::vector<EnumDescriptor> enums;
    std::vector<Function> functions;
    std::optional<std::uint32_t> globalInitializerFunction;
    std::optional<std::uint32_t> entryFunction;
};

[[nodiscard]] std::string disassemble(const BytecodeModule& module);

} // namespace serenity::bytecode
