#include "serenity/bytecode/instruction.hpp"

namespace serenity::bytecode {

std::string_view opcodeName(Opcode opcode) noexcept {
    switch (opcode) {
    case Opcode::Nop: return "NOP";
    case Opcode::PushConstant: return "PUSH_CONST";
    case Opcode::PushEnum: return "PUSH_ENUM";
    case Opcode::LoadLocal: return "LOAD_LOCAL";
    case Opcode::StoreLocal: return "STORE_LOCAL";
    case Opcode::LoadGlobal: return "LOAD_GLOBAL";
    case Opcode::StoreGlobal: return "STORE_GLOBAL";
    case Opcode::Pop: return "POP";
    case Opcode::Duplicate: return "DUP";
    case Opcode::NewStruct: return "NEW_STRUCT";
    case Opcode::GetField: return "GET_FIELD";
    case Opcode::SetField: return "SET_FIELD";
    case Opcode::Add: return "ADD";
    case Opcode::Subtract: return "SUB";
    case Opcode::Multiply: return "MUL";
    case Opcode::Divide: return "DIV";
    case Opcode::Modulo: return "MOD";
    case Opcode::Negate: return "NEG";
    case Opcode::LogicalNot: return "NOT";
    case Opcode::BitwiseNot: return "BIT_NOT";
    case Opcode::CompareEqual: return "CMP_EQ";
    case Opcode::CompareNotEqual: return "CMP_NE";
    case Opcode::CompareLess: return "CMP_LT";
    case Opcode::CompareLessEqual: return "CMP_LE";
    case Opcode::CompareGreater: return "CMP_GT";
    case Opcode::CompareGreaterEqual: return "CMP_GE";
    case Opcode::BitwiseAnd: return "BIT_AND";
    case Opcode::BitwiseOr: return "BIT_OR";
    case Opcode::BitwiseXor: return "BIT_XOR";
    case Opcode::ShiftLeft: return "SHL";
    case Opcode::ShiftRight: return "SHR";
    case Opcode::Jump: return "JUMP";
    case Opcode::JumpIfFalse: return "JUMP_IF_FALSE";
    case Opcode::Call: return "CALL";
    case Opcode::Return: return "RETURN";
    case Opcode::Print: return "PRINT";
    case Opcode::Println: return "PRINTLN";
    }
    return "UNKNOWN";
}

std::string_view valueTypeName(ValueType type) noexcept {
    switch (type) {
    case ValueType::Invalid: return "invalid";
    case ValueType::Void: return "void";
    case ValueType::Null: return "null";
    case ValueType::Int: return "int";
    case ValueType::Float: return "float";
    case ValueType::Double: return "double";
    case ValueType::Bool: return "bool";
    case ValueType::String: return "string";
    case ValueType::Struct: return "struct";
    case ValueType::Enum: return "enum";
    }
    return "unknown";
}

} // namespace serenity::bytecode
