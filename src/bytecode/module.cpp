#include "serenity/bytecode/module.hpp"

#include <iomanip>
#include <sstream>

namespace serenity::bytecode {

std::string disassemble(const BytecodeModule& module) {
    std::ostringstream output;
    output << "Serenity bytecode " << module.versionMajor << '.' << module.versionMinor << "\n";

    output << "\nConstants (" << module.constants.size() << "):\n";
    for (std::size_t index = 0; index < module.constants.size(); ++index) {
        const Constant& constant = module.constants[index];
        output << "  #" << index << " " << valueTypeName(constant.type) << " = ";
        if (std::holds_alternative<std::monostate>(constant.value)) {
            output << "null";
        } else if (const auto* integerValue = std::get_if<std::int64_t>(&constant.value)) {
            output << *integerValue;
        } else if (const auto* floatValue = std::get_if<float>(&constant.value)) {
            output << *floatValue << 'f';
        } else if (const auto* doubleValue = std::get_if<double>(&constant.value)) {
            output << *doubleValue;
        } else if (const auto* boolValue = std::get_if<bool>(&constant.value)) {
            output << (*boolValue ? "true" : "false");
        } else if (const auto* stringValue = std::get_if<std::string>(&constant.value)) {
            output << std::quoted(*stringValue);
        } else if (const auto* enumValue = std::get_if<EnumConstantValue>(&constant.value)) {
            output << "enum(" << enumValue->enumIndex << ", " << enumValue->memberIndex << ')';
        }
        output << '\n';
    }

    output << "\nGlobals (" << module.globals.size() << "):\n";
    for (std::size_t index = 0; index < module.globals.size(); ++index) {
        const GlobalSlot& global = module.globals[index];
        output << "  G" << index << ' ' << global.type.nominalName;
        if (global.type.nominalName.empty()) output << valueTypeName(global.type.kind);
        output << ' ' << global.name << (global.isMutable ? " mut" : " const") << '\n';
    }

    output << "\nStructs (" << module.structs.size() << "):\n";
    for (std::size_t index = 0; index < module.structs.size(); ++index) {
        const StructDescriptor& structure = module.structs[index];
        output << "  S" << index << ' ' << structure.name << " {";
        for (const FieldDescriptor& field : structure.fields) {
            output << ' ' << valueTypeName(field.type.kind) << ' ' << field.name << ';';
        }
        output << " }\n";
    }

    output << "\nEnums (" << module.enums.size() << "):\n";
    for (std::size_t index = 0; index < module.enums.size(); ++index) {
        const EnumDescriptor& enumeration = module.enums[index];
        output << "  E" << index << ' ' << enumeration.name << " {";
        for (std::size_t member = 0; member < enumeration.members.size(); ++member) {
            output << ' ' << enumeration.members[member] << '=' << member;
        }
        output << " }\n";
    }

    output << "\nFunctions (" << module.functions.size() << "):\n";
    for (std::size_t functionIndex = 0; functionIndex < module.functions.size(); ++functionIndex) {
        const Function& function = module.functions[functionIndex];
        output << "\nF" << functionIndex << ' ' << function.name << "(";
        for (std::size_t parameter = 0; parameter < function.parameterTypes.size(); ++parameter) {
            if (parameter != 0) output << ", ";
            const TypeInfo& type = function.parameterTypes[parameter];
            output << (type.nominalName.empty() ? valueTypeName(type.kind) : type.nominalName);
        }
        output << ") -> "
               << (function.returnType.nominalName.empty()
                       ? valueTypeName(function.returnType.kind)
                       : std::string_view(function.returnType.nominalName))
               << " [locals=" << function.locals.size() << "]\n";

        for (std::size_t instructionIndex = 0; instructionIndex < function.code.size(); ++instructionIndex) {
            const Instruction& instruction = function.code[instructionIndex];
            output << "  " << std::setw(4) << instructionIndex << "  "
                   << std::left << std::setw(16) << opcodeName(instruction.opcode) << std::right;
            if (instruction.type != ValueType::Invalid) {
                output << std::setw(8) << valueTypeName(instruction.type);
            } else {
                output << std::setw(8) << "";
            }
            if (instruction.operand != 0 || instruction.extra != 0 ||
                instruction.opcode == Opcode::PushConstant ||
                instruction.opcode == Opcode::LoadLocal ||
                instruction.opcode == Opcode::StoreLocal ||
                instruction.opcode == Opcode::LoadGlobal ||
                instruction.opcode == Opcode::StoreGlobal ||
                instruction.opcode == Opcode::Jump ||
                instruction.opcode == Opcode::JumpIfFalse ||
                instruction.opcode == Opcode::Call ||
                instruction.opcode == Opcode::NewStruct ||
                instruction.opcode == Opcode::PushEnum ||
                instruction.opcode == Opcode::GetField ||
                instruction.opcode == Opcode::SetField) {
                output << " " << instruction.operand;
                if (instruction.extra != 0 || instruction.opcode == Opcode::Call ||
                    instruction.opcode == Opcode::PushEnum) {
                    output << ", " << instruction.extra;
                }
            }
            const ast::SourceSpan& span = function.debugLocations[instructionIndex];
            if (span.begin.line != 0) {
                output << "    ; " << span.begin.line << ':' << span.begin.column;
            }
            output << '\n';
        }
    }

    if (module.globalInitializerFunction.has_value()) {
        output << "\nGlobal initializer: F" << *module.globalInitializerFunction << '\n';
    }
    if (module.entryFunction.has_value()) {
        output << "Entry point: F" << *module.entryFunction << '\n';
    }
    return output.str();
}

} // namespace serenity::bytecode
