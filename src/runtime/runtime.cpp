#include "serenity/aot/runtime_abi.hpp"
#include "serenity/bytecode/instruction.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {

using serenity::aot::runtime::SnValue;
using serenity::bytecode::Opcode;
using serenity::bytecode::ValueType;

struct RuntimeStruct {
    std::vector<SnValue> fields;
};

std::vector<std::unique_ptr<RuntimeStruct>>& allocations() {
    static std::vector<std::unique_ptr<RuntimeStruct>> objects;
    return objects;
}

[[noreturn]] void runtimeFailure(const char* message) {
    std::cerr << "Serenity runtime error: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

[[nodiscard]] std::int64_t asInt(const SnValue& value) {
    if (value.tag != static_cast<std::uint64_t>(ValueType::Int)) {
        runtimeFailure("expected an int value");
    }
    return std::bit_cast<std::int64_t>(value.payload);
}

[[nodiscard]] float asFloat(const SnValue& value) {
    if (value.tag != static_cast<std::uint64_t>(ValueType::Float)) {
        runtimeFailure("expected a float value");
    }
    return std::bit_cast<float>(static_cast<std::uint32_t>(value.payload));
}

[[nodiscard]] double asDouble(const SnValue& value) {
    if (value.tag != static_cast<std::uint64_t>(ValueType::Double)) {
        runtimeFailure("expected a double value");
    }
    return std::bit_cast<double>(value.payload);
}

[[nodiscard]] bool asBool(const SnValue& value) {
    if (value.tag != static_cast<std::uint64_t>(ValueType::Bool)) {
        runtimeFailure("expected a bool value");
    }
    return value.payload != 0;
}

[[nodiscard]] const char* asString(const SnValue& value) {
    if (value.tag != static_cast<std::uint64_t>(ValueType::String) || value.payload == 0) {
        runtimeFailure("expected a non-null string value");
    }
    return reinterpret_cast<const char*>(static_cast<std::uintptr_t>(value.payload));
}

[[nodiscard]] bool equalValues(const SnValue& left, const SnValue& right) {
    if (left.tag == static_cast<std::uint64_t>(ValueType::Null) ||
        right.tag == static_cast<std::uint64_t>(ValueType::Null)) {
        return left.tag == static_cast<std::uint64_t>(ValueType::Null) &&
               right.tag == static_cast<std::uint64_t>(ValueType::Null);
    }
    if (left.tag != right.tag) return false;
    switch (static_cast<ValueType>(left.tag)) {
    case ValueType::Bool:
    case ValueType::Int:
    case ValueType::Float:
    case ValueType::Double:
    case ValueType::Enum:
        return left.payload == right.payload;
    case ValueType::String:
        if (left.payload == 0 || right.payload == 0) return left.payload == right.payload;
        return std::strcmp(asString(left), asString(right)) == 0;
    case ValueType::Struct:
        return left.payload == right.payload;
    case ValueType::Null:
        return true;
    case ValueType::Invalid:
    case ValueType::Void:
        return false;
    }
    return false;
}

[[nodiscard]] bool compareInt(Opcode opcode, std::int64_t a, std::int64_t b) {
    switch (opcode) {
    case Opcode::CompareEqual: return a == b;
    case Opcode::CompareNotEqual: return a != b;
    case Opcode::CompareLess: return a < b;
    case Opcode::CompareLessEqual: return a <= b;
    case Opcode::CompareGreater: return a > b;
    case Opcode::CompareGreaterEqual: return a >= b;
    default: runtimeFailure("invalid comparison opcode");
    }
}

[[nodiscard]] bool compareFloat(Opcode opcode, float a, float b) {
    switch (opcode) {
    case Opcode::CompareEqual: return a == b;
    case Opcode::CompareNotEqual: return a != b;
    case Opcode::CompareLess: return a < b;
    case Opcode::CompareLessEqual: return a <= b;
    case Opcode::CompareGreater: return a > b;
    case Opcode::CompareGreaterEqual: return a >= b;
    default: runtimeFailure("invalid comparison opcode");
    }
}

[[nodiscard]] bool compareDouble(Opcode opcode, double a, double b) {
    switch (opcode) {
    case Opcode::CompareEqual: return a == b;
    case Opcode::CompareNotEqual: return a != b;
    case Opcode::CompareLess: return a < b;
    case Opcode::CompareLessEqual: return a <= b;
    case Opcode::CompareGreater: return a > b;
    case Opcode::CompareGreaterEqual: return a >= b;
    default: runtimeFailure("invalid comparison opcode");
    }
}

void setBool(SnValue* out, bool value) {
    *out = SnValue{static_cast<std::uint64_t>(ValueType::Bool), value ? 1U : 0U};
}

[[nodiscard]] RuntimeStruct& checkedStruct(void* object, std::int32_t fieldIndex) {
    if (object == nullptr) runtimeFailure("field access on null struct");
    if (fieldIndex < 0) runtimeFailure("negative struct field index");
    auto& structure = *static_cast<RuntimeStruct*>(object);
    if (static_cast<std::size_t>(fieldIndex) >= structure.fields.size()) {
        runtimeFailure("struct field index is out of range");
    }
    return structure;
}

} // namespace

extern "C" void sn_rt_make(SnValue* out, std::uint64_t tag, std::uint64_t payload) {
    if (out == nullptr) runtimeFailure("invalid output slot");
    *out = SnValue{tag, payload};
}

extern "C" void sn_rt_default(SnValue* out, std::uint64_t tag) {
    if (out == nullptr) runtimeFailure("invalid default-value slot");
    const auto type = static_cast<ValueType>(tag);
    switch (type) {
    case ValueType::Int:
    case ValueType::Float:
    case ValueType::Double:
    case ValueType::Bool:
        *out = SnValue{tag, 0};
        return;
    case ValueType::String:
    case ValueType::Struct:
    case ValueType::Enum:
    case ValueType::Null:
        *out = SnValue{static_cast<std::uint64_t>(ValueType::Null), 0};
        return;
    case ValueType::Void:
    case ValueType::Invalid:
        *out = SnValue{static_cast<std::uint64_t>(ValueType::Null), 0};
        return;
    }
    runtimeFailure("unknown type in default initialization");
}

extern "C" void sn_rt_unary(SnValue* out, std::int32_t opcodeValue,
                             std::int32_t typeValue, const SnValue* input) {
    if (out == nullptr || input == nullptr) runtimeFailure("invalid unary-operation slot");
    const SnValue value = *input; // output may alias input
    const auto opcode = static_cast<Opcode>(opcodeValue);
    const auto type = static_cast<ValueType>(typeValue);
    switch (opcode) {
    case Opcode::Negate:
        if (type == ValueType::Int) {
            const std::int64_t integer = asInt(value);
            if (integer == std::numeric_limits<std::int64_t>::min()) runtimeFailure("integer negation overflow");
            *out = SnValue{static_cast<std::uint64_t>(ValueType::Int), static_cast<std::uint64_t>(-integer)};
            return;
        }
        if (type == ValueType::Float) {
            const float result = -asFloat(value);
            *out = SnValue{static_cast<std::uint64_t>(ValueType::Float),
                           std::bit_cast<std::uint32_t>(result)};
            return;
        }
        if (type == ValueType::Double) {
            const double result = -asDouble(value);
            *out = SnValue{static_cast<std::uint64_t>(ValueType::Double), std::bit_cast<std::uint64_t>(result)};
            return;
        }
        runtimeFailure("unsupported type for unary minus");
    case Opcode::LogicalNot:
        setBool(out, !asBool(value));
        return;
    case Opcode::BitwiseNot:
        *out = SnValue{static_cast<std::uint64_t>(ValueType::Int),
                       ~static_cast<std::uint64_t>(asInt(value))};
        return;
    default:
        runtimeFailure("unknown unary opcode");
    }
}

extern "C" void sn_rt_binary(SnValue* out, std::int32_t opcodeValue,
                              std::int32_t typeValue, const SnValue* leftPtr,
                              const SnValue* rightPtr) {
    if (out == nullptr || leftPtr == nullptr || rightPtr == nullptr) {
        runtimeFailure("invalid binary-operation slot");
    }
    const SnValue left = *leftPtr;
    const SnValue right = *rightPtr; // output may alias left
    const auto opcode = static_cast<Opcode>(opcodeValue);
    const auto type = static_cast<ValueType>(typeValue);

    if (opcode == Opcode::CompareEqual || opcode == Opcode::CompareNotEqual) {
        bool equal = false;
        if (left.tag == static_cast<std::uint64_t>(ValueType::Null) ||
            right.tag == static_cast<std::uint64_t>(ValueType::Null)) {
            equal = left.tag == static_cast<std::uint64_t>(ValueType::Null) &&
                    right.tag == static_cast<std::uint64_t>(ValueType::Null);
        } else if (type == ValueType::Int) {
            equal = asInt(left) == asInt(right);
        } else if (type == ValueType::Float) {
            equal = asFloat(left) == asFloat(right);
        } else if (type == ValueType::Double) {
            equal = asDouble(left) == asDouble(right);
        } else {
            equal = equalValues(left, right);
        }
        setBool(out, opcode == Opcode::CompareEqual ? equal : !equal);
        return;
    }

    if (type == ValueType::Int) {
        const std::int64_t a = asInt(left);
        const std::int64_t b = asInt(right);
        if (opcode >= Opcode::CompareLess && opcode <= Opcode::CompareGreaterEqual) {
            setBool(out, compareInt(opcode, a, b));
            return;
        }
        std::int64_t result = 0;
        switch (opcode) {
        case Opcode::Add:
            if (__builtin_add_overflow(a, b, &result)) runtimeFailure("integer addition overflow");
            break;
        case Opcode::Subtract:
            if (__builtin_sub_overflow(a, b, &result)) runtimeFailure("integer subtraction overflow");
            break;
        case Opcode::Multiply:
            if (__builtin_mul_overflow(a, b, &result)) runtimeFailure("integer multiplication overflow");
            break;
        case Opcode::Divide:
            if (b == 0) runtimeFailure("integer division by zero");
            if (a == std::numeric_limits<std::int64_t>::min() && b == -1) runtimeFailure("integer division overflow");
            result = a / b;
            break;
        case Opcode::Modulo:
            if (b == 0) runtimeFailure("integer remainder by zero");
            if (a == std::numeric_limits<std::int64_t>::min() && b == -1) runtimeFailure("integer remainder overflow");
            result = a % b;
            break;
        case Opcode::BitwiseAnd: result = a & b; break;
        case Opcode::BitwiseOr: result = a | b; break;
        case Opcode::BitwiseXor: result = a ^ b; break;
        case Opcode::ShiftLeft:
            if (b < 0 || b >= 63 || a < 0 ||
                static_cast<std::uint64_t>(a) >
                    (static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) >> static_cast<unsigned>(b))) {
                runtimeFailure("invalid or overflowing left shift");
            }
            result = static_cast<std::int64_t>(static_cast<std::uint64_t>(a) << static_cast<unsigned>(b));
            break;
        case Opcode::ShiftRight:
            if (b < 0 || b >= 63) runtimeFailure("invalid right-shift count");
            result = a >> static_cast<unsigned>(b);
            break;
        default: runtimeFailure("invalid integer binary operation");
        }
        *out = SnValue{static_cast<std::uint64_t>(ValueType::Int), static_cast<std::uint64_t>(result)};
        return;
    }

    if (type == ValueType::Float) {
        const float a = asFloat(left);
        const float b = asFloat(right);
        if (opcode >= Opcode::CompareLess && opcode <= Opcode::CompareGreaterEqual) {
            setBool(out, compareFloat(opcode, a, b)); return;
        }
        float result = 0.0F;
        switch (opcode) {
        case Opcode::Add: result = a + b; break;
        case Opcode::Subtract: result = a - b; break;
        case Opcode::Multiply: result = a * b; break;
        case Opcode::Divide: result = a / b; break;
        default: runtimeFailure("invalid float binary operation");
        }
        *out = SnValue{static_cast<std::uint64_t>(ValueType::Float), std::bit_cast<std::uint32_t>(result)};
        return;
    }

    if (type == ValueType::Double) {
        const double a = asDouble(left);
        const double b = asDouble(right);
        if (opcode >= Opcode::CompareLess && opcode <= Opcode::CompareGreaterEqual) {
            setBool(out, compareDouble(opcode, a, b)); return;
        }
        double result = 0.0;
        switch (opcode) {
        case Opcode::Add: result = a + b; break;
        case Opcode::Subtract: result = a - b; break;
        case Opcode::Multiply: result = a * b; break;
        case Opcode::Divide: result = a / b; break;
        default: runtimeFailure("invalid double binary operation");
        }
        *out = SnValue{static_cast<std::uint64_t>(ValueType::Double), std::bit_cast<std::uint64_t>(result)};
        return;
    }

    runtimeFailure("unsupported binary operation in AOT runtime");
}

extern "C" void sn_rt_print(const SnValue* value, bool newline) {
    if (value == nullptr) runtimeFailure("invalid PRINT value slot");
    switch (static_cast<ValueType>(value->tag)) {
    case ValueType::Int:
        std::cout << static_cast<std::int64_t>(value->payload);
        break;
    case ValueType::Float:
        std::cout << std::bit_cast<float>(static_cast<std::uint32_t>(value->payload));
        break;
    case ValueType::Double:
        std::cout << std::bit_cast<double>(value->payload);
        break;
    case ValueType::Bool:
        std::cout << (value->payload != 0 ? "true" : "false");
        break;
    case ValueType::String:
        if (value->payload == 0) runtimeFailure("attempted to print null");
        std::cout << reinterpret_cast<const char*>(static_cast<std::uintptr_t>(value->payload));
        break;
    default:
        runtimeFailure("PRINT received a value that is not a printable primitive");
    }
    if (newline) std::cout << '\n';
}

extern "C" void* sn_rt_struct_new(std::int32_t fieldCount) {
    if (fieldCount < 0) runtimeFailure("negative struct field count");
    try {
        auto object = std::make_unique<RuntimeStruct>();
        object->fields.resize(static_cast<std::size_t>(fieldCount),
                              SnValue{static_cast<std::uint64_t>(ValueType::Null), 0});
        RuntimeStruct* raw = object.get();
        allocations().push_back(std::move(object));
        return raw;
    } catch (const std::bad_alloc&) {
        runtimeFailure("out of memory allocating struct");
    }
}

extern "C" void sn_rt_struct_default(void* object, std::int32_t fieldIndex, std::uint64_t typeTag) {
    RuntimeStruct& structure = checkedStruct(object, fieldIndex);
    SnValue value;
    sn_rt_default(&value, typeTag);
    structure.fields[static_cast<std::size_t>(fieldIndex)] = value;
}

extern "C" void sn_rt_struct_set_value(void* object, std::int32_t fieldIndex,
                                        std::uint64_t tag, std::uint64_t payload) {
    RuntimeStruct& structure = checkedStruct(object, fieldIndex);
    structure.fields[static_cast<std::size_t>(fieldIndex)] = SnValue{tag, payload};
}

extern "C" void sn_rt_struct_get(const SnValue* objectValue, std::int32_t fieldIndex, SnValue* out) {
    if (objectValue == nullptr || out == nullptr) runtimeFailure("invalid struct field access slot");
    const SnValue objectCopy = *objectValue;
    if (objectCopy.tag != static_cast<std::uint64_t>(ValueType::Struct) || objectCopy.payload == 0) {
        runtimeFailure("field access on null or non-struct value");
    }
    void* object = reinterpret_cast<void*>(static_cast<std::uintptr_t>(objectCopy.payload));
    RuntimeStruct& structure = checkedStruct(object, fieldIndex);
    *out = structure.fields[static_cast<std::size_t>(fieldIndex)];
}

extern "C" void sn_rt_struct_set(const SnValue* objectValue, std::int32_t fieldIndex, const SnValue* value) {
    if (objectValue == nullptr || value == nullptr) runtimeFailure("invalid struct field assignment slot");
    const SnValue objectCopy = *objectValue;
    const SnValue valueCopy = *value;
    if (objectCopy.tag != static_cast<std::uint64_t>(ValueType::Struct) || objectCopy.payload == 0) {
        runtimeFailure("field assignment on null or non-struct value");
    }
    void* object = reinterpret_cast<void*>(static_cast<std::uintptr_t>(objectCopy.payload));
    RuntimeStruct& structure = checkedStruct(object, fieldIndex);
    structure.fields[static_cast<std::size_t>(fieldIndex)] = valueCopy;
}

extern "C" std::int32_t sn_rt_exit_code(const SnValue* value) {
    if (value == nullptr || value->tag != static_cast<std::uint64_t>(ValueType::Int)) return 0;
    return static_cast<std::int32_t>(static_cast<std::uint8_t>(value->payload));
}
