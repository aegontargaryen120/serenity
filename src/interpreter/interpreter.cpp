#include "serenity/interpreter/interpreter.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <ostream>
#include <sstream>
#include <type_traits>
#include <utility>

namespace serenity::interpreter {
namespace {

using bytecode::Opcode;
using bytecode::TypeInfo;
using bytecode::ValueType;

[[nodiscard]] bool isNullable(ValueType type) noexcept {
    return type == ValueType::Null || type == ValueType::String ||
           type == ValueType::Struct || type == ValueType::Enum;
}

[[nodiscard]] const char* typeLabel(ValueType type) noexcept {
    return bytecode::valueTypeName(type).data();
}

[[nodiscard]] std::optional<std::uint32_t> findStructIndex(
    const bytecode::BytecodeModule& module, const std::string& name) {
    for (std::size_t index = 0; index < module.structs.size(); ++index) {
        if (module.structs[index].name == name) return static_cast<std::uint32_t>(index);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::uint32_t> findEnumIndex(
    const bytecode::BytecodeModule& module, const std::string& name) {
    for (std::size_t index = 0; index < module.enums.size(); ++index) {
        if (module.enums[index].name == name) return static_cast<std::uint32_t>(index);
    }
    return std::nullopt;
}

[[nodiscard]] bool checkedAdd(std::int64_t left, std::int64_t right, std::int64_t& result) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return !__builtin_add_overflow(left, right, &result);
#else
    if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
        (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) return false;
    result = left + right;
    return true;
#endif
}

[[nodiscard]] bool checkedSubtract(std::int64_t left, std::int64_t right, std::int64_t& result) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return !__builtin_sub_overflow(left, right, &result);
#else
    if ((right < 0 && left > std::numeric_limits<std::int64_t>::max() + right) ||
        (right > 0 && left < std::numeric_limits<std::int64_t>::min() + right)) return false;
    result = left - right;
    return true;
#endif
}

[[nodiscard]] bool checkedMultiply(std::int64_t left, std::int64_t right, std::int64_t& result) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return !__builtin_mul_overflow(left, right, &result);
#else
    if (left == 0 || right == 0) { result = 0; return true; }
    if (left == -1 && right == std::numeric_limits<std::int64_t>::min()) return false;
    if (right == -1 && left == std::numeric_limits<std::int64_t>::min()) return false;
    const std::int64_t candidate = left * right;
    if (candidate / right != left) return false;
    result = candidate;
    return true;
#endif
}

} // namespace

Interpreter::Interpreter(
    const bytecode::BytecodeModule& module, std::ostream& output, InterpreterOptions options)
    : module_(module), output_(output), options_(options) {}

ExecutionResult Interpreter::run() {
    reset();
    globals_.reserve(module_.globals.size());
    for (const bytecode::GlobalSlot& global : module_.globals) {
        globals_.push_back(defaultValue(global.type));
    }

    if (!module_.entryFunction.has_value()) {
        error_ = RuntimeError{"bytecode module has no 'main' entry function", 0, "<module>", 0, {}};
        return {false, std::nullopt, error_, stats_};
    }

    if (module_.globalInitializerFunction.has_value()) {
        if (!executeFunction(*module_.globalInitializerFunction, {}, 0)) {
            return {false, std::nullopt, error_, stats_};
        }
    }

    if (!executeFunction(*module_.entryFunction, {}, 0)) {
        return {false, std::nullopt, error_, stats_};
    }

    return {true, rootReturnValue_, std::nullopt, stats_};
}

void Interpreter::reset() {
    operandStack_.clear();
    globals_.clear();
    frames_.clear();
    stats_ = {};
    error_.reset();
    rootReturnValue_.reset();
}

bool Interpreter::executeFunction(
    std::uint32_t functionIndex, std::vector<Value> arguments, std::size_t stackBase) {
    const std::size_t stopDepth = frames_.size();
    if (!pushFrame(functionIndex, std::move(arguments), stackBase)) return false;
    return executeLoop(stopDepth);
}

bool Interpreter::pushFrame(
    std::uint32_t functionIndex, std::vector<Value> arguments, std::size_t stackBase) {
    if (functionIndex >= module_.functions.size()) {
        error_ = RuntimeError{"function index is outside the bytecode module", functionIndex,
                              "<invalid>", 0, {}};
        return false;
    }
    if (frames_.size() >= options_.maxCallDepth) {
        const bytecode::Function& function = module_.functions[functionIndex];
        error_ = RuntimeError{"maximum call depth exceeded", functionIndex, function.name, 0, {}};
        return false;
    }

    const bytecode::Function& function = module_.functions[functionIndex];
    if (arguments.size() != function.parameterTypes.size()) {
        error_ = RuntimeError{
            "function '" + function.name + "' expects " +
                std::to_string(function.parameterTypes.size()) + " argument(s), received " +
                std::to_string(arguments.size()),
            functionIndex, function.name, 0, {}};
        return false;
    }
    if (function.locals.size() < function.parameterTypes.size()) {
        error_ = RuntimeError{"function local-slot table is smaller than its parameter list",
                              functionIndex, function.name, 0, {}};
        return false;
    }

    Frame frame;
    frame.functionIndex = functionIndex;
    frame.stackBase = stackBase;
    frame.locals.reserve(function.locals.size());
    for (const bytecode::LocalSlot& local : function.locals) {
        frame.locals.push_back(defaultValue(local.type));
    }
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (!valueMatchesType(arguments[index], function.parameterTypes[index])) {
            error_ = RuntimeError{
                "argument " + std::to_string(index + 1) + " passed to '" + function.name +
                    "' has an incompatible runtime type",
                functionIndex, function.name, 0, {}};
            return false;
        }
        frame.locals[index] = std::move(arguments[index]);
    }

    frames_.push_back(std::move(frame));
    ++stats_.functionCalls;
    if (frames_.size() > stats_.peakCallDepth) stats_.peakCallDepth = frames_.size();
    return true;
}

bool Interpreter::executeLoop(std::size_t stopDepth) {
    while (frames_.size() > stopDepth) {
        Frame& frame = frames_.back();
        if (frame.functionIndex >= module_.functions.size()) {
            fail("active frame has an invalid function index", frame.functionIndex,
                 frame.instructionPointer);
            return false;
        }
        const bytecode::Function& function = module_.functions[frame.functionIndex];
        if (frame.instructionPointer >= function.code.size()) {
            fail("instruction pointer moved past the end of function '" + function.name +
                     "' without RETURN", frame.functionIndex, function.code.size());
            return false;
        }
        if (options_.maxInstructions != 0 && stats_.executedInstructions >= options_.maxInstructions) {
            fail("maximum instruction count exceeded", frame.functionIndex, frame.instructionPointer);
            return false;
        }

        const std::size_t instructionIndex = frame.instructionPointer++;
        const bytecode::Instruction instruction = function.code[instructionIndex];
        ++stats_.executedInstructions;
        if (!executeInstruction(instruction, frame.functionIndex, instructionIndex)) return false;
        if (operandStack_.size() > stats_.peakOperandStack) {
            stats_.peakOperandStack = operandStack_.size();
        }
    }
    return true;
}

void Interpreter::fail(
    std::string message, std::uint32_t functionIndex, std::size_t instructionIndex) {
    std::string functionName = "<invalid>";
    ast::SourceSpan span {};
    if (functionIndex < module_.functions.size()) {
        const bytecode::Function& function = module_.functions[functionIndex];
        functionName = function.name;
        if (instructionIndex < function.debugLocations.size()) {
            span = function.debugLocations[instructionIndex];
        }
    }
    error_ = RuntimeError{std::move(message), functionIndex, std::move(functionName), instructionIndex, span};
}

bool Interpreter::pop(Value& value, std::uint32_t functionIndex, std::size_t instructionIndex) {
    if (operandStack_.empty()) {
        fail("operand stack underflow", functionIndex, instructionIndex);
        return false;
    }
    value = std::move(operandStack_.back());
    operandStack_.pop_back();
    return true;
}

bool Interpreter::popTwo(
    Value& left, Value& right, std::uint32_t functionIndex, std::size_t instructionIndex) {
    if (!pop(right, functionIndex, instructionIndex)) return false;
    return pop(left, functionIndex, instructionIndex);
}

void Interpreter::push(Value value) {
    operandStack_.push_back(std::move(value));
}

Value Interpreter::defaultValue(const TypeInfo& type) const {
    switch (type.kind) {
    case ValueType::Int: return Value{std::int64_t{0}};
    case ValueType::Float: return Value{0.0F};
    case ValueType::Double: return Value{0.0};
    case ValueType::Bool: return Value{false};
    case ValueType::Null:
    case ValueType::String:
    case ValueType::Struct:
    case ValueType::Enum:
    case ValueType::Void:
    case ValueType::Invalid:
        return Value{};
    }
    return Value{};
}

Value Interpreter::constantValue(const bytecode::Constant& constant) const {
    if (std::holds_alternative<std::monostate>(constant.value)) return Value{};
    if (const auto* value = std::get_if<std::int64_t>(&constant.value)) return Value{*value};
    if (const auto* value = std::get_if<float>(&constant.value)) return Value{*value};
    if (const auto* value = std::get_if<double>(&constant.value)) return Value{*value};
    if (const auto* value = std::get_if<bool>(&constant.value)) return Value{*value};
    if (const auto* value = std::get_if<std::string>(&constant.value)) return Value{*value};
    if (const auto* value = std::get_if<bytecode::EnumConstantValue>(&constant.value)) {
        return Value{EnumValue{value->enumIndex, value->memberIndex}};
    }
    return Value{};
}

bool Interpreter::valueMatchesType(const Value& value, const TypeInfo& type) const {
    if (std::holds_alternative<std::monostate>(value.data)) return isNullable(type.kind);
    switch (type.kind) {
    case ValueType::Int: return std::holds_alternative<std::int64_t>(value.data);
    case ValueType::Float: return std::holds_alternative<float>(value.data);
    case ValueType::Double: return std::holds_alternative<double>(value.data);
    case ValueType::Bool: return std::holds_alternative<bool>(value.data);
    case ValueType::String: return std::holds_alternative<std::string>(value.data);
    case ValueType::Enum: {
        const auto* enumeration = std::get_if<EnumValue>(&value.data);
        if (enumeration == nullptr || enumeration->enumIndex >= module_.enums.size()) return false;
        const auto expectedIndex = findEnumIndex(module_, type.nominalName);
        return expectedIndex.has_value() && enumeration->enumIndex == *expectedIndex;
    }
    case ValueType::Struct: {
        const auto* structure = std::get_if<std::shared_ptr<StructInstance>>(&value.data);
        if (structure == nullptr || !*structure || (*structure)->descriptorIndex >= module_.structs.size()) return false;
        const auto expectedIndex = findStructIndex(module_, type.nominalName);
        return expectedIndex.has_value() && (*structure)->descriptorIndex == *expectedIndex;
    }
    case ValueType::Null: return std::holds_alternative<std::monostate>(value.data);
    case ValueType::Void:
    case ValueType::Invalid: return false;
    }
    return false;
}

bool Interpreter::executeInstruction(
    const bytecode::Instruction& instruction, std::uint32_t functionIndex,
    std::size_t instructionIndex) {
    switch (instruction.opcode) {
    case Opcode::Nop:
        return true;

    case Opcode::PushConstant: {
        if (instruction.operand < 0 ||
            static_cast<std::size_t>(instruction.operand) >= module_.constants.size()) {
            fail("constant-pool index is out of range", functionIndex, instructionIndex);
            return false;
        }
        push(constantValue(module_.constants[static_cast<std::size_t>(instruction.operand)]));
        return true;
    }

    case Opcode::PushEnum: {
        if (instruction.operand < 0 || instruction.extra < 0 ||
            static_cast<std::size_t>(instruction.operand) >= module_.enums.size()) {
            fail("enum constant index is out of range", functionIndex, instructionIndex);
            return false;
        }
        const auto enumIndex = static_cast<std::size_t>(instruction.operand);
        const auto memberIndex = static_cast<std::size_t>(instruction.extra);
        if (memberIndex >= module_.enums[enumIndex].members.size()) {
            fail("enum member index is out of range", functionIndex, instructionIndex);
            return false;
        }
        push(Value{EnumValue{static_cast<std::uint32_t>(enumIndex),
                             static_cast<std::uint32_t>(memberIndex)}});
        return true;
    }

    case Opcode::LoadLocal: {
        Frame& frame = frames_.back();
        if (instruction.operand < 0 || static_cast<std::size_t>(instruction.operand) >= frame.locals.size()) {
            fail("local slot index is out of range", functionIndex, instructionIndex);
            return false;
        }
        push(frame.locals[static_cast<std::size_t>(instruction.operand)]);
        return true;
    }

    case Opcode::StoreLocal: {
        Frame& frame = frames_.back();
        if (instruction.operand < 0 || static_cast<std::size_t>(instruction.operand) >= frame.locals.size()) {
            fail("local slot index is out of range", functionIndex, instructionIndex);
            return false;
        }
        Value value;
        if (!pop(value, functionIndex, instructionIndex)) return false;
        const bytecode::LocalSlot& slot = module_.functions[functionIndex].locals[
            static_cast<std::size_t>(instruction.operand)];
        if (!valueMatchesType(value, slot.type)) {
            fail("value stored in local '" + slot.name + "' does not match " + typeLabel(slot.type.kind),
                 functionIndex, instructionIndex);
            return false;
        }
        frame.locals[static_cast<std::size_t>(instruction.operand)] = std::move(value);
        return true;
    }

    case Opcode::LoadGlobal: {
        if (instruction.operand < 0 || static_cast<std::size_t>(instruction.operand) >= globals_.size()) {
            fail("global slot index is out of range", functionIndex, instructionIndex);
            return false;
        }
        push(globals_[static_cast<std::size_t>(instruction.operand)]);
        return true;
    }

    case Opcode::StoreGlobal: {
        if (instruction.operand < 0 || static_cast<std::size_t>(instruction.operand) >= globals_.size()) {
            fail("global slot index is out of range", functionIndex, instructionIndex);
            return false;
        }
        Value value;
        if (!pop(value, functionIndex, instructionIndex)) return false;
        const bytecode::GlobalSlot& slot = module_.globals[static_cast<std::size_t>(instruction.operand)];
        if (!valueMatchesType(value, slot.type)) {
            fail("value stored in global '" + slot.name + "' does not match " + typeLabel(slot.type.kind),
                 functionIndex, instructionIndex);
            return false;
        }
        globals_[static_cast<std::size_t>(instruction.operand)] = std::move(value);
        return true;
    }

    case Opcode::Pop: {
        Value ignored;
        return pop(ignored, functionIndex, instructionIndex);
    }

    case Opcode::Duplicate:
        if (operandStack_.empty()) {
            fail("DUP requires a value on the operand stack", functionIndex, instructionIndex);
            return false;
        }
        push(operandStack_.back());
        return true;

    case Opcode::NewStruct:
        return executeNewStruct(instruction, functionIndex, instructionIndex);

    case Opcode::GetField:
    case Opcode::SetField:
        return executeField(instruction, functionIndex, instructionIndex);

    case Opcode::Add:
    case Opcode::Subtract:
    case Opcode::Multiply:
    case Opcode::Divide:
    case Opcode::Modulo:
    case Opcode::Negate:
        return executeArithmetic(instruction, functionIndex, instructionIndex);

    case Opcode::LogicalNot: {
        Value value;
        if (!pop(value, functionIndex, instructionIndex)) return false;
        const auto* boolean = std::get_if<bool>(&value.data);
        if (boolean == nullptr) {
            fail("logical NOT expected a bool operand", functionIndex, instructionIndex);
            return false;
        }
        push(Value{!*boolean});
        return true;
    }

    case Opcode::BitwiseNot: {
        Value value;
        if (!pop(value, functionIndex, instructionIndex)) return false;
        const auto* integer = std::get_if<std::int64_t>(&value.data);
        if (integer == nullptr) {
            fail("bitwise NOT expected an int operand", functionIndex, instructionIndex);
            return false;
        }
        push(Value{static_cast<std::int64_t>(~static_cast<std::uint64_t>(*integer))});
        return true;
    }

    case Opcode::CompareEqual:
    case Opcode::CompareNotEqual:
    case Opcode::CompareLess:
    case Opcode::CompareLessEqual:
    case Opcode::CompareGreater:
    case Opcode::CompareGreaterEqual:
        return executeComparison(instruction, functionIndex, instructionIndex);

    case Opcode::BitwiseAnd:
    case Opcode::BitwiseOr:
    case Opcode::BitwiseXor:
    case Opcode::ShiftLeft:
    case Opcode::ShiftRight:
        return executeBitwise(instruction, functionIndex, instructionIndex);

    case Opcode::Jump:
    case Opcode::JumpIfFalse: {
        Frame& frame = frames_.back();
        bool shouldJump = true;
        if (instruction.opcode == Opcode::JumpIfFalse) {
            Value condition;
            if (!pop(condition, functionIndex, instructionIndex)) return false;
            const auto* boolean = std::get_if<bool>(&condition.data);
            if (boolean == nullptr) {
                fail("conditional jump expected a bool value", functionIndex, instructionIndex);
                return false;
            }
            shouldJump = !*boolean;
        }
        if (shouldJump) {
            if (instruction.operand < 0 ||
                static_cast<std::size_t>(instruction.operand) > module_.functions[functionIndex].code.size()) {
                fail("jump target is outside the current function", functionIndex, instructionIndex);
                return false;
            }
            frame.instructionPointer = static_cast<std::size_t>(instruction.operand);
        }
        return true;
    }

    case Opcode::Call:
        return executeCall(instruction, functionIndex, instructionIndex);

    case Opcode::Return:
        return executeReturn(instruction, functionIndex, instructionIndex);

    case Opcode::Print:
    case Opcode::Println: {
        Value value;
        if (!pop(value, functionIndex, instructionIndex)) return false;
        if (std::holds_alternative<std::monostate>(value.data)) {
            fail("attempted to print null", functionIndex, instructionIndex);
            return false;
        }
        if (const auto* integer = std::get_if<std::int64_t>(&value.data)) output_ << *integer;
        else if (const auto* floatValue = std::get_if<float>(&value.data)) output_ << *floatValue;
        else if (const auto* doubleValue = std::get_if<double>(&value.data)) output_ << *doubleValue;
        else if (const auto* boolean = std::get_if<bool>(&value.data)) output_ << (*boolean ? "true" : "false");
        else if (const auto* string = std::get_if<std::string>(&value.data)) output_ << *string;
        else {
            fail("PRINT received a value that is not a printable primitive", functionIndex, instructionIndex);
            return false;
        }
        if (instruction.opcode == Opcode::Println) output_ << '\n';
        return true;
    }
    }

    fail("unknown opcode", functionIndex, instructionIndex);
    return false;
}

bool Interpreter::executeArithmetic(
    const bytecode::Instruction& instruction, std::uint32_t functionIndex,
    std::size_t instructionIndex) {
    if (instruction.opcode == Opcode::Negate) {
        Value operand;
        if (!pop(operand, functionIndex, instructionIndex)) return false;
        if (instruction.type == ValueType::Int) {
            const auto* value = std::get_if<std::int64_t>(&operand.data);
            if (value == nullptr || *value == std::numeric_limits<std::int64_t>::min()) {
                fail("integer negation overflow or invalid operand", functionIndex, instructionIndex);
                return false;
            }
            push(Value{-*value});
            return true;
        }
        if (instruction.type == ValueType::Float) {
            const auto* value = std::get_if<float>(&operand.data);
            if (value == nullptr) { fail("NEG expected float", functionIndex, instructionIndex); return false; }
            push(Value{-*value});
            return true;
        }
        if (instruction.type == ValueType::Double) {
            const auto* value = std::get_if<double>(&operand.data);
            if (value == nullptr) { fail("NEG expected double", functionIndex, instructionIndex); return false; }
            push(Value{-*value});
            return true;
        }
        fail("NEG received an unsupported type", functionIndex, instructionIndex);
        return false;
    }

    Value left;
    Value right;
    if (!popTwo(left, right, functionIndex, instructionIndex)) return false;

    if (instruction.type == ValueType::Int) {
        const auto* a = std::get_if<std::int64_t>(&left.data);
        const auto* b = std::get_if<std::int64_t>(&right.data);
        if (a == nullptr || b == nullptr) { fail("integer arithmetic received non-integer operands", functionIndex, instructionIndex); return false; }
        std::int64_t result = 0;
        switch (instruction.opcode) {
        case Opcode::Add:
            if (!checkedAdd(*a, *b, result)) { fail("integer addition overflow", functionIndex, instructionIndex); return false; }
            break;
        case Opcode::Subtract:
            if (!checkedSubtract(*a, *b, result)) { fail("integer subtraction overflow", functionIndex, instructionIndex); return false; }
            break;
        case Opcode::Multiply:
            if (!checkedMultiply(*a, *b, result)) { fail("integer multiplication overflow", functionIndex, instructionIndex); return false; }
            break;
        case Opcode::Divide:
            if (*b == 0) { fail("integer division by zero", functionIndex, instructionIndex); return false; }
            if (*a == std::numeric_limits<std::int64_t>::min() && *b == -1) { fail("integer division overflow", functionIndex, instructionIndex); return false; }
            result = *a / *b;
            break;
        case Opcode::Modulo:
            if (*b == 0) { fail("integer remainder by zero", functionIndex, instructionIndex); return false; }
            if (*a == std::numeric_limits<std::int64_t>::min() && *b == -1) { fail("integer remainder overflow", functionIndex, instructionIndex); return false; }
            result = *a % *b;
            break;
        default: fail("invalid integer arithmetic opcode", functionIndex, instructionIndex); return false;
        }
        push(Value{result});
        return true;
    }

    if (instruction.type == ValueType::Float) {
        const auto* a = std::get_if<float>(&left.data);
        const auto* b = std::get_if<float>(&right.data);
        if (a == nullptr || b == nullptr) { fail("float arithmetic received incompatible operands", functionIndex, instructionIndex); return false; }
        switch (instruction.opcode) {
        case Opcode::Add: push(Value{*a + *b}); return true;
        case Opcode::Subtract: push(Value{*a - *b}); return true;
        case Opcode::Multiply: push(Value{*a * *b}); return true;
        case Opcode::Divide: push(Value{*a / *b}); return true;
        default: fail("invalid float arithmetic opcode", functionIndex, instructionIndex); return false;
        }
    }

    if (instruction.type == ValueType::Double) {
        const auto* a = std::get_if<double>(&left.data);
        const auto* b = std::get_if<double>(&right.data);
        if (a == nullptr || b == nullptr) { fail("double arithmetic received incompatible operands", functionIndex, instructionIndex); return false; }
        switch (instruction.opcode) {
        case Opcode::Add: push(Value{*a + *b}); return true;
        case Opcode::Subtract: push(Value{*a - *b}); return true;
        case Opcode::Multiply: push(Value{*a * *b}); return true;
        case Opcode::Divide: push(Value{*a / *b}); return true;
        default: fail("invalid double arithmetic opcode", functionIndex, instructionIndex); return false;
        }
    }

    fail("arithmetic instruction has an unsupported type tag", functionIndex, instructionIndex);
    return false;
}

bool Interpreter::isComparison(Opcode opcode) noexcept {
    return opcode == Opcode::CompareEqual || opcode == Opcode::CompareNotEqual ||
           opcode == Opcode::CompareLess || opcode == Opcode::CompareLessEqual ||
           opcode == Opcode::CompareGreater || opcode == Opcode::CompareGreaterEqual;
}

bool Interpreter::executeComparison(
    const bytecode::Instruction& instruction, std::uint32_t functionIndex,
    std::size_t instructionIndex) {
    Value left;
    Value right;
    if (!popTwo(left, right, functionIndex, instructionIndex)) return false;

    bool result = false;
    const bool equality = instruction.opcode == Opcode::CompareEqual ||
                          instruction.opcode == Opcode::CompareNotEqual;
    bool comparable = false;

    if (instruction.type == ValueType::Int) {
        const auto* a = std::get_if<std::int64_t>(&left.data);
        const auto* b = std::get_if<std::int64_t>(&right.data);
        if (a && b) { comparable = true; if (instruction.opcode == Opcode::CompareEqual) result = *a == *b;
            else if (instruction.opcode == Opcode::CompareNotEqual) result = *a != *b;
            else if (instruction.opcode == Opcode::CompareLess) result = *a < *b;
            else if (instruction.opcode == Opcode::CompareLessEqual) result = *a <= *b;
            else if (instruction.opcode == Opcode::CompareGreater) result = *a > *b;
            else result = *a >= *b; }
    } else if (instruction.type == ValueType::Float) {
        const auto* a = std::get_if<float>(&left.data); const auto* b = std::get_if<float>(&right.data);
        if (a && b) { comparable = true; if (instruction.opcode == Opcode::CompareEqual) result = *a == *b;
            else if (instruction.opcode == Opcode::CompareNotEqual) result = *a != *b;
            else if (instruction.opcode == Opcode::CompareLess) result = *a < *b;
            else if (instruction.opcode == Opcode::CompareLessEqual) result = *a <= *b;
            else if (instruction.opcode == Opcode::CompareGreater) result = *a > *b;
            else result = *a >= *b; }
    } else if (instruction.type == ValueType::Double) {
        const auto* a = std::get_if<double>(&left.data); const auto* b = std::get_if<double>(&right.data);
        if (a && b) { comparable = true; if (instruction.opcode == Opcode::CompareEqual) result = *a == *b;
            else if (instruction.opcode == Opcode::CompareNotEqual) result = *a != *b;
            else if (instruction.opcode == Opcode::CompareLess) result = *a < *b;
            else if (instruction.opcode == Opcode::CompareLessEqual) result = *a <= *b;
            else if (instruction.opcode == Opcode::CompareGreater) result = *a > *b;
            else result = *a >= *b; }
    } else if (equality) {
        const bool leftNull = std::holds_alternative<std::monostate>(left.data);
        const bool rightNull = std::holds_alternative<std::monostate>(right.data);
        if (leftNull || rightNull) {
            // Serenity permits null comparisons for nullable types. A null
            // value is equal only to another null value.
            comparable = true;
            result = leftNull && rightNull;
        } else if (const auto* boolLeft = std::get_if<bool>(&left.data)) {
            if (const auto* boolRight = std::get_if<bool>(&right.data)) {
                comparable = true; result = *boolLeft == *boolRight;
            }
        } else if (const auto* stringLeft = std::get_if<std::string>(&left.data)) {
            if (const auto* stringRight = std::get_if<std::string>(&right.data)) {
                comparable = true; result = *stringLeft == *stringRight;
            }
        } else if (const auto* enumLeft = std::get_if<EnumValue>(&left.data)) {
            if (const auto* enumRight = std::get_if<EnumValue>(&right.data)) {
                comparable = true; result = *enumLeft == *enumRight;
            }
        } else if (const auto* structLeft = std::get_if<std::shared_ptr<StructInstance>>(&left.data)) {
            if (const auto* structRight = std::get_if<std::shared_ptr<StructInstance>>(&right.data)) {
                comparable = true; result = *structLeft == *structRight;
            }
        }
        if (instruction.opcode == Opcode::CompareNotEqual) result = !result;
    }

    if (!comparable) {
        fail("comparison instruction received incompatible runtime operands", functionIndex, instructionIndex);
        return false;
    }
    push(Value{result});
    return true;
}

bool Interpreter::isBitwise(Opcode opcode) noexcept {
    return opcode == Opcode::BitwiseAnd || opcode == Opcode::BitwiseOr ||
           opcode == Opcode::BitwiseXor || opcode == Opcode::ShiftLeft ||
           opcode == Opcode::ShiftRight;
}

bool Interpreter::executeBitwise(
    const bytecode::Instruction& instruction, std::uint32_t functionIndex,
    std::size_t instructionIndex) {
    Value left; Value right;
    if (!popTwo(left, right, functionIndex, instructionIndex)) return false;
    const auto* a = std::get_if<std::int64_t>(&left.data);
    const auto* b = std::get_if<std::int64_t>(&right.data);
    if (a == nullptr || b == nullptr) { fail("bitwise operation expected int operands", functionIndex, instructionIndex); return false; }

    std::int64_t result = 0;
    switch (instruction.opcode) {
    case Opcode::BitwiseAnd: result = *a & *b; break;
    case Opcode::BitwiseOr: result = *a | *b; break;
    case Opcode::BitwiseXor: result = *a ^ *b; break;
    case Opcode::ShiftLeft: {
        if (*b < 0 || *b >= 63 || *a < 0 ||
            static_cast<std::uint64_t>(*a) > (static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) >> static_cast<unsigned>(*b))) {
            fail("invalid or overflowing left shift", functionIndex, instructionIndex);
            return false;
        }
        result = static_cast<std::int64_t>(static_cast<std::uint64_t>(*a) << static_cast<unsigned>(*b));
        break;
    }
    case Opcode::ShiftRight:
        if (*b < 0 || *b >= 63) { fail("invalid right-shift count", functionIndex, instructionIndex); return false; }
        result = *a >> static_cast<unsigned>(*b);
        break;
    default: fail("invalid bitwise opcode", functionIndex, instructionIndex); return false;
    }
    push(Value{result});
    return true;
}

bool Interpreter::executeNewStruct(
    const bytecode::Instruction& instruction, std::uint32_t functionIndex,
    std::size_t instructionIndex) {
    if (instruction.operand < 0 || static_cast<std::size_t>(instruction.operand) >= module_.structs.size()) {
        fail("struct descriptor index is out of range", functionIndex, instructionIndex);
        return false;
    }
    const auto descriptorIndex = static_cast<std::uint32_t>(instruction.operand);
    const bytecode::StructDescriptor& descriptor = module_.structs[descriptorIndex];
    auto instance = std::make_shared<StructInstance>();
    instance->descriptorIndex = descriptorIndex;
    instance->fields.reserve(descriptor.fields.size());
    for (const bytecode::FieldDescriptor& field : descriptor.fields) {
        if (field.defaultConstant.has_value()) {
            const std::uint32_t constantIndex = *field.defaultConstant;
            if (constantIndex >= module_.constants.size()) {
                fail("struct field default constant index is out of range", functionIndex, instructionIndex);
                return false;
            }
            Value value = constantValue(module_.constants[constantIndex]);
            if (!valueMatchesType(value, field.type)) {
                fail("struct field default has an incompatible constant type", functionIndex, instructionIndex);
                return false;
            }
            instance->fields.push_back(std::move(value));
        } else {
            instance->fields.push_back(defaultValue(field.type));
        }
    }
    push(Value{std::move(instance)});
    return true;
}

bool Interpreter::executeField(
    const bytecode::Instruction& instruction, std::uint32_t functionIndex,
    std::size_t instructionIndex) {
    if (instruction.opcode == Opcode::GetField) {
        Value object;
        if (!pop(object, functionIndex, instructionIndex)) return false;
        const auto* reference = std::get_if<std::shared_ptr<StructInstance>>(&object.data);
        if (reference == nullptr || !*reference) { fail("field access on null or non-struct value", functionIndex, instructionIndex); return false; }
        if (instruction.operand < 0 || static_cast<std::size_t>(instruction.operand) >= (*reference)->fields.size()) {
            fail("struct field index is out of range", functionIndex, instructionIndex);
            return false;
        }
        push((*reference)->fields[static_cast<std::size_t>(instruction.operand)]);
        return true;
    }

    Value value;
    Value object;
    if (!pop(value, functionIndex, instructionIndex) || !pop(object, functionIndex, instructionIndex)) return false;
    const auto* reference = std::get_if<std::shared_ptr<StructInstance>>(&object.data);
    if (reference == nullptr || !*reference) { fail("field assignment on null or non-struct value", functionIndex, instructionIndex); return false; }
    const std::size_t fieldIndex = instruction.operand < 0
        ? (*reference)->fields.size()
        : static_cast<std::size_t>(instruction.operand);
    if (fieldIndex >= (*reference)->fields.size()) { fail("struct field index is out of range", functionIndex, instructionIndex); return false; }
    const std::uint32_t descriptorIndex = (*reference)->descriptorIndex;
    if (descriptorIndex >= module_.structs.size() || fieldIndex >= module_.structs[descriptorIndex].fields.size()) {
        fail("struct instance descriptor is invalid", functionIndex, instructionIndex);
        return false;
    }
    const bytecode::FieldDescriptor& field = module_.structs[descriptorIndex].fields[fieldIndex];
    if (!valueMatchesType(value, field.type)) {
        fail("assigned value does not match struct field '" + field.name + "'", functionIndex, instructionIndex);
        return false;
    }
    (*reference)->fields[fieldIndex] = std::move(value);
    return true;
}

bool Interpreter::executeCall(
    const bytecode::Instruction& instruction, std::uint32_t functionIndex,
    std::size_t instructionIndex) {
    if (instruction.operand < 0 || static_cast<std::size_t>(instruction.operand) >= module_.functions.size()) {
        fail("CALL target function index is out of range", functionIndex, instructionIndex);
        return false;
    }
    if (instruction.extra < 0) { fail("CALL argument count is negative", functionIndex, instructionIndex); return false; }
    const std::size_t argumentCount = static_cast<std::size_t>(instruction.extra);
    if (argumentCount > operandStack_.size()) { fail("CALL does not have enough arguments on the operand stack", functionIndex, instructionIndex); return false; }
    const std::size_t base = operandStack_.size() - argumentCount;
    std::vector<Value> arguments;
    arguments.reserve(argumentCount);
    for (std::size_t index = base; index < operandStack_.size(); ++index) arguments.push_back(operandStack_[index]);
    operandStack_.resize(base);
    return pushFrame(static_cast<std::uint32_t>(instruction.operand), std::move(arguments), base);
}

bool Interpreter::executeReturn(
    const bytecode::Instruction& instruction, std::uint32_t functionIndex,
    std::size_t instructionIndex) {
    const bytecode::Function& function = module_.functions[functionIndex];
    Value returnValue;
    if (function.returnType.kind != ValueType::Void) {
        if (!pop(returnValue, functionIndex, instructionIndex)) return false;
        if (!valueMatchesType(returnValue, function.returnType)) {
            fail("RETURN value does not match the declared return type of '" + function.name + "'",
                 functionIndex, instructionIndex);
            return false;
        }
    }

    const std::size_t stackBase = frames_.back().stackBase;
    if (stackBase > operandStack_.size()) { fail("invalid call-frame operand-stack base", functionIndex, instructionIndex); return false; }
    operandStack_.resize(stackBase);
    frames_.pop_back();

    if (frames_.empty()) {
        if (function.returnType.kind != ValueType::Void) rootReturnValue_ = std::move(returnValue);
    } else if (function.returnType.kind != ValueType::Void) {
        push(std::move(returnValue));
    }
    (void)instruction;
    return true;
}

} // namespace serenity::interpreter
