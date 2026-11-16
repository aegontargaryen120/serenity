#pragma once

#include <cstdint>
#include <string_view>
#include <type_traits>

namespace serenity::bytecode {

// Values used by the stack VM. Integer values are represented as signed 64-bit
// quantities in bytecode; the language's eventual ABI can impose a narrower
// range without changing the instruction encoding.
enum class ValueType : std::uint8_t {
    Invalid = 0,
    Void,
    Null,
    Int,
    Float,
    Double,
    Bool,
    String,
    Struct,
    Enum,
    Any,
    Stream,
};

enum class Opcode : std::uint8_t {
    Nop = 0,
    PushConstant,
    PushEnum,
    LoadLocal,
    StoreLocal,
    LoadGlobal,
    StoreGlobal,
    Pop,
    Duplicate,
    NewStruct,
    GetField,
    SetField,

    Add,
    Subtract,
    Multiply,
    Divide,
    Modulo,
    Negate,
    LogicalNot,
    BitwiseNot,
    CompareEqual,
    CompareNotEqual,
    CompareLess,
    CompareLessEqual,
    CompareGreater,
    CompareGreaterEqual,
    BitwiseAnd,
    BitwiseOr,
    BitwiseXor,
    ShiftLeft,
    ShiftRight,

    Jump,
    JumpIfFalse,
    Call,
    Return,
    Print,
    Println,
    PushStream,
    StreamRead,
    StreamWrite,
};

// Fixed-width logical instruction layout: 12 bytes. `type` carries the value
// type for typed operations; `operand` and `extra` are opcode-specific indices,
// counts, or jump targets. Do not serialize this struct by dumping its memory:
// the future file format must write each field explicitly in little-endian order.
struct Instruction {
    Opcode opcode {Opcode::Nop};
    ValueType type {ValueType::Invalid};
    std::uint16_t flags {0};
    std::int32_t operand {0};
    std::int32_t extra {0};
};

static_assert(std::is_standard_layout_v<Instruction>);
static_assert(sizeof(Instruction) == 12, "Serenity bytecode instructions must be 12 bytes");

[[nodiscard]] std::string_view opcodeName(Opcode opcode) noexcept;
[[nodiscard]] std::string_view valueTypeName(ValueType type) noexcept;

} // namespace serenity::bytecode
