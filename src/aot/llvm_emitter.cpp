#include "serenity/aot/llvm_emitter.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <queue>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace serenity::aot {
namespace {

using bytecode::BytecodeModule;
using bytecode::Constant;
using bytecode::EnumConstantValue;
using bytecode::Function;
using bytecode::Instruction;
using bytecode::Opcode;
using bytecode::ValueType;

struct StackAnalysis {
    std::vector<std::optional<std::int64_t>> depthBefore;
    std::int64_t maxDepth {0};
    std::vector<std::string> errors;
};

struct Effect {
    std::int64_t required {0};
    std::int64_t delta {0};
};

[[nodiscard]] Effect stackEffect(
    const BytecodeModule& module, const Function& function, const Instruction& instruction,
    std::string& error) {
    switch (instruction.opcode) {
    case Opcode::Nop:
    case Opcode::Jump:
        return {};
    case Opcode::PushConstant:
    case Opcode::PushEnum:
    case Opcode::LoadLocal:
    case Opcode::LoadGlobal:
    case Opcode::NewStruct:
        return {0, 1};
    case Opcode::StoreLocal:
    case Opcode::StoreGlobal:
    case Opcode::Pop:
    case Opcode::Print:
    case Opcode::Println:
    case Opcode::JumpIfFalse:
        return {1, -1};
    case Opcode::Duplicate:
        return {1, 1};
    case Opcode::GetField:
        return {1, 0};
    case Opcode::SetField:
        return {2, -2};
    case Opcode::Add:
    case Opcode::Subtract:
    case Opcode::Multiply:
    case Opcode::Divide:
    case Opcode::Modulo:
    case Opcode::CompareEqual:
    case Opcode::CompareNotEqual:
    case Opcode::CompareLess:
    case Opcode::CompareLessEqual:
    case Opcode::CompareGreater:
    case Opcode::CompareGreaterEqual:
    case Opcode::BitwiseAnd:
    case Opcode::BitwiseOr:
    case Opcode::BitwiseXor:
    case Opcode::ShiftLeft:
    case Opcode::ShiftRight:
        return {2, -1};
    case Opcode::Negate:
    case Opcode::LogicalNot:
    case Opcode::BitwiseNot:
        return {1, 0};
    case Opcode::Call: {
        if (instruction.operand < 0 ||
            static_cast<std::size_t>(instruction.operand) >= module.functions.size()) {
            error = "CALL references an invalid function index";
            return {};
        }
        if (instruction.extra < 0) {
            error = "CALL has a negative argument count";
            return {};
        }
        const auto& target = module.functions[static_cast<std::size_t>(instruction.operand)];
        const auto argc = static_cast<std::int64_t>(instruction.extra);
        if (static_cast<std::size_t>(instruction.extra) != target.parameterTypes.size()) {
            error = "CALL argument count does not match function '" + target.name + "'";
            return {};
        }
        return {argc, target.returnType.kind == ValueType::Void ? -argc : 1 - argc};
    }
    case Opcode::Return:
        return function.returnType.kind == ValueType::Void ? Effect{} : Effect{1, -1};
    }
    error = "unsupported bytecode opcode";
    return {};
}

[[nodiscard]] StackAnalysis analyzeStack(const BytecodeModule& module, const Function& function) {
    StackAnalysis result;
    const std::size_t count = function.code.size();
    result.depthBefore.resize(count + 1);
    std::queue<std::size_t> work;
    result.depthBefore[0] = 0;
    work.push(0);

    auto propagate = [&](std::size_t target, std::int64_t depth) {
        if (target > count) {
            result.errors.push_back("control-flow target is outside function '" + function.name + "'");
            return;
        }
        auto& existing = result.depthBefore[target];
        if (!existing.has_value()) {
            existing = depth;
            work.push(target);
        } else if (*existing != depth) {
            result.errors.push_back("inconsistent operand-stack depth at control-flow merge in function '" +
                                    function.name + "'");
        }
    };

    while (!work.empty()) {
        const std::size_t pc = work.front();
        work.pop();
        if (pc == count) continue;
        const std::int64_t depth = *result.depthBefore[pc];
        std::string effectError;
        const Instruction& instruction = function.code[pc];
        const Effect effect = stackEffect(module, function, instruction, effectError);
        if (!effectError.empty()) {
            result.errors.push_back(std::move(effectError));
            continue;
        }
        if (depth < effect.required) {
            result.errors.push_back("operand-stack underflow in function '" + function.name + "'");
            continue;
        }
        const std::int64_t nextDepth = depth + effect.delta;
        if (nextDepth < 0) {
            result.errors.push_back("negative operand-stack depth in function '" + function.name + "'");
            continue;
        }
        if (nextDepth > result.maxDepth) result.maxDepth = nextDepth;

        if (instruction.opcode == Opcode::Return) continue;
        if (instruction.opcode == Opcode::Jump || instruction.opcode == Opcode::JumpIfFalse) {
            if (instruction.operand < 0) {
                result.errors.push_back("negative jump target in function '" + function.name + "'");
                continue;
            }
            propagate(static_cast<std::size_t>(instruction.operand), nextDepth);
            if (instruction.opcode == Opcode::Jump) continue;
        }
        propagate(pc + 1, nextDepth);
    }
    return result;
}

[[nodiscard]] std::string llvmByteString(const std::string& input) {
    std::ostringstream out;
    for (char raw : input) {
        const auto ch = static_cast<unsigned char>(raw);
        if (ch >= 0x20U && ch <= 0x7EU && ch != '"' && ch != '\\') {
            out << static_cast<char>(ch);
        } else {
            out << '\\' << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<unsigned int>(ch) << std::nouppercase << std::dec;
        }
    }
    out << "\\00";
    return out.str();
}

[[nodiscard]] std::uint64_t constantPayload(const Constant& constant) {
    if (const auto* value = std::get_if<std::int64_t>(&constant.value)) {
        return static_cast<std::uint64_t>(*value);
    }
    if (const auto* value = std::get_if<float>(&constant.value)) {
        return static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(*value));
    }
    if (const auto* value = std::get_if<double>(&constant.value)) {
        return std::bit_cast<std::uint64_t>(*value);
    }
    if (const auto* value = std::get_if<bool>(&constant.value)) return *value ? 1U : 0U;
    if (const auto* value = std::get_if<EnumConstantValue>(&constant.value)) {
        return (static_cast<std::uint64_t>(value->enumIndex) << 32U) |
               static_cast<std::uint64_t>(value->memberIndex);
    }
    return 0;
}

[[nodiscard]] std::string valueTypeNumber(ValueType type) {
    return std::to_string(static_cast<unsigned int>(type));
}

class Emitter {
public:
    explicit Emitter(const BytecodeModule& module) : module_(module) {}

    LLVMResult run() {
        if (!module_.entryFunction.has_value()) {
            errors_.push_back({"program has no 'main' function", {}});
            return {std::nullopt, std::move(errors_)};
        }
        if (*module_.entryFunction >= module_.functions.size()) {
            errors_.push_back({"module entry function index is invalid", {}});
            return {std::nullopt, std::move(errors_)};
        }
        const Function& entry = module_.functions[*module_.entryFunction];
        if (!entry.parameterTypes.empty()) {
            errors_.push_back({"AOT entry function 'main' must not take parameters", {}});
            return {std::nullopt, std::move(errors_)};
        }
        if (entry.returnType.kind != ValueType::Int && entry.returnType.kind != ValueType::Void) {
            errors_.push_back({"AOT entry function 'main' must return int (or have no return value)", {}});
            return {std::nullopt, std::move(errors_)};
        }
        if (module_.globalInitializerFunction.has_value() &&
            *module_.globalInitializerFunction >= module_.functions.size()) {
            errors_.push_back({"module global initializer function index is invalid", {}});
            return {std::nullopt, std::move(errors_)};
        }

        analyses_.reserve(module_.functions.size());
        for (const Function& function : module_.functions) {
            analyses_.push_back(analyzeStack(module_, function));
            for (const std::string& error : analyses_.back().errors) errors_.push_back({error, {}});
        }
        if (!errors_.empty()) return {std::nullopt, std::move(errors_)};

        emitHeader();
        emitStringGlobals();
        emitGlobalStorage();
        emitRuntimeDeclarations();
        for (std::size_t index = 0; index < module_.functions.size(); ++index) {
            emitFunction(index);
        }
        emitNativeMain();
        if (!errors_.empty()) return {std::nullopt, std::move(errors_)};
        return {out_.str(), {}};
    }

private:
    const BytecodeModule& module_;
    std::vector<StackAnalysis> analyses_;
    std::vector<LLVMError> errors_;
    std::ostringstream out_;
    std::size_t temp_ {0};

    [[nodiscard]] std::string temp() { return "%t" + std::to_string(temp_++); }
    [[nodiscard]] std::string localPtr(std::uint32_t index) const {
        return "%local_" + std::to_string(index);
    }
    [[nodiscard]] std::string stackPtr(std::int64_t index) const {
        return "%stack_" + std::to_string(index);
    }
    [[nodiscard]] std::string globalPtr(std::uint32_t index) {
        const std::string p = temp();
        out_ << "  " << p << " = getelementptr inbounds [" << std::max<std::size_t>(1, module_.globals.size())
             << " x %SnValue], ptr @sn_globals, i32 0, i32 " << index << '\n';
        return p;
    }
    [[nodiscard]] std::string stringPointer(std::uint32_t constantIndex) {
        const std::string p = temp();
        const std::string global = "@sn_string_" + std::to_string(constantIndex);
        const std::string& value = std::get<std::string>(module_.constants[constantIndex].value);
        out_ << "  " << p << " = getelementptr inbounds [" << (value.size() + 1)
             << " x i8], ptr " << global << ", i64 0, i64 0\n";
        return p;
    }
    [[nodiscard]] std::string callArgsPointer(std::size_t pc, std::size_t count) {
        const std::string p = temp();
        const std::size_t allocCount = std::max<std::size_t>(1, count);
        out_ << "  " << p << " = getelementptr inbounds [" << allocCount
             << " x %SnValue], ptr %call_args_" << pc << ", i32 0, i32 0\n";
        return p;
    }

    void emitHeader() {
        out_ << "; Serenity AOT module, generated from bytecode v" << module_.versionMajor << '.'
             << module_.versionMinor << "\n"
             << "source_filename = \"Serenity\"\n\n"
             << "%SnValue = type { i64, i64 }\n\n";
    }

    void emitStringGlobals() {
        for (std::size_t index = 0; index < module_.constants.size(); ++index) {
            const Constant& constant = module_.constants[index];
            const auto* string = std::get_if<std::string>(&constant.value);
            if (constant.type != ValueType::String || string == nullptr) continue;
            out_ << "@sn_string_" << index << " = private unnamed_addr constant ["
                 << (string->size() + 1) << " x i8] c\"" << llvmByteString(*string)
                 << "\", align 1\n";
        }
        if (!module_.constants.empty()) out_ << '\n';
    }

    void emitGlobalStorage() {
        out_ << "@sn_globals = internal global [" << std::max<std::size_t>(1, module_.globals.size())
             << " x %SnValue] zeroinitializer, align 8\n\n";
    }

    void emitRuntimeDeclarations() {
        out_ << "declare void @sn_rt_make(ptr, i64, i64)\n"
             << "declare void @sn_rt_default(ptr, i64)\n"
             << "declare void @sn_rt_unary(ptr, i32, i32, ptr)\n"
             << "declare void @sn_rt_binary(ptr, i32, i32, ptr, ptr)\n"
             << "declare void @sn_rt_print(ptr, i1)\n"
             << "declare ptr @sn_rt_struct_new(i32)\n"
             << "declare void @sn_rt_struct_default(ptr, i32, i64)\n"
             << "declare void @sn_rt_struct_set_value(ptr, i32, i64, i64)\n"
             << "declare void @sn_rt_struct_get(ptr, i32, ptr)\n"
             << "declare void @sn_rt_struct_set(ptr, i32, ptr)\n"
             << "declare i32 @sn_rt_exit_code(ptr)\n\n";
    }

    void emitFunction(std::size_t functionIndex) {
        const Function& function = module_.functions[functionIndex];
        const StackAnalysis& analysis = analyses_[functionIndex];
        temp_ = 0;
        out_ << "define internal void @sn_fn_" << functionIndex << "(ptr %args, ptr %out) {\nentry:\n";

        const std::size_t stackAllocation = static_cast<std::size_t>(std::max<std::int64_t>(1, analysis.maxDepth + 2));
        out_ << "  %locals = alloca [" << std::max<std::size_t>(1, function.locals.size())
             << " x %SnValue], align 8\n"
             << "  %stack = alloca [" << stackAllocation
             << " x %SnValue], align 8\n";
        for (std::size_t index = 0; index < stackAllocation; ++index) {
            out_ << "  %stack_" << index << " = getelementptr inbounds [" << stackAllocation
                 << " x %SnValue], ptr %stack, i32 0, i32 " << index << '\n';
        }

        for (std::size_t pc = 0; pc < function.code.size(); ++pc) {
            const Instruction& instruction = function.code[pc];
            if (instruction.opcode != Opcode::Call || instruction.extra < 0) continue;
            const std::size_t argc = static_cast<std::size_t>(instruction.extra);
            out_ << "  %call_args_" << pc << " = alloca [" << std::max<std::size_t>(1, argc)
                 << " x %SnValue], align 8\n"
                 << "  %call_out_" << pc << " = alloca %SnValue, align 8\n";
        }
        for (std::size_t index = 0; index < function.locals.size(); ++index) {
            const std::string ptr = localPtr(static_cast<std::uint32_t>(index));
            out_ << "  " << ptr << " = getelementptr inbounds ["
                 << std::max<std::size_t>(1, function.locals.size())
                 << " x %SnValue], ptr %locals, i32 0, i32 " << index << '\n';
            out_ << "  call void @sn_rt_default(ptr " << ptr << ", i64 "
                 << valueTypeNumber(function.locals[index].type.kind) << ")\n";
        }
        for (std::size_t index = 0; index < function.parameterTypes.size(); ++index) {
            const std::string argPtr = temp();
            const std::string argValue = temp();
            out_ << "  " << argPtr << " = getelementptr inbounds %SnValue, ptr %args, i64 " << index << '\n'
                 << "  " << argValue << " = load %SnValue, ptr " << argPtr << ", align 8\n"
                 << "  store %SnValue " << argValue << ", ptr " << localPtr(static_cast<std::uint32_t>(index))
                 << ", align 8\n";
        }
        if (function.code.empty()) {
            emitDefaultReturn(function);
            out_ << "}\n\n";
            return;
        }
        out_ << "  br label %bb0\n\n";
        for (std::size_t pc = 0; pc < function.code.size(); ++pc) {
            if (!analysis.depthBefore[pc].has_value()) continue;
            out_ << "bb" << pc << ":\n";
            emitInstruction(functionIndex, pc, *analysis.depthBefore[pc]);
        }
        if (analysis.depthBefore[function.code.size()].has_value()) {
            out_ << "bbend:\n";
            emitDefaultReturn(function);
        }
        out_ << "}\n\n";
    }

    void emitDefaultReturn(const Function& function) {
        if (function.returnType.kind != ValueType::Void) {
            out_ << "  call void @sn_rt_default(ptr %out, i64 "
                 << valueTypeNumber(function.returnType.kind) << ")\n";
        }
        out_ << "  ret void\n";
    }

    void emitConstantTo(const Constant& constant, std::string_view destination, std::uint32_t indexHint) {
        const std::uint64_t tag = static_cast<std::uint64_t>(constant.type);
        if (constant.type == ValueType::String && std::holds_alternative<std::string>(constant.value)) {
            const auto p = stringPointer(indexHint);
            const auto raw = temp();
            out_ << "  " << raw << " = ptrtoint ptr " << p << " to i64\n"
                 << "  call void @sn_rt_make(ptr " << destination << ", i64 " << tag
                 << ", i64 " << raw << ")\n";
        } else {
            out_ << "  call void @sn_rt_make(ptr " << destination << ", i64 " << tag
                 << ", i64 " << constantPayload(constant) << ")\n";
        }
    }

    void emitInstruction(std::size_t functionIndex, std::size_t pc, std::int64_t depth) {
        const Function& function = module_.functions[functionIndex];
        const Instruction& instruction = function.code[pc];
        const auto next = [&]() { out_ << "  br label %bb" << (pc + 1) << "\n\n"; };
        const std::string dst = depth >= 0 ? stackPtr(depth) : "";
        auto stackAt = [&](std::int64_t index) { return stackPtr(index); };

        switch (instruction.opcode) {
        case Opcode::Nop:
            next();
            return;
        case Opcode::PushConstant: {
            if (instruction.operand < 0 || static_cast<std::size_t>(instruction.operand) >= module_.constants.size()) {
                errors_.push_back({"PUSH_CONST references an invalid constant", {}}); return;
            }
            const auto index = static_cast<std::uint32_t>(instruction.operand);
            emitConstantTo(module_.constants[index], dst, index);
            next(); return;
        }
        case Opcode::PushEnum: {
            if (instruction.operand < 0 || instruction.extra < 0) {
                errors_.push_back({"PUSH_ENUM has an invalid enum operand", {}}); return;
            }
            const std::uint64_t payload =
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(instruction.operand)) << 32U) |
                static_cast<std::uint32_t>(instruction.extra);
            out_ << "  call void @sn_rt_make(ptr " << dst << ", i64 "
                 << valueTypeNumber(ValueType::Enum) << ", i64 " << payload << ")\n";
            next(); return;
        }
        case Opcode::LoadLocal:
        case Opcode::StoreLocal: {
            if (instruction.operand < 0 || static_cast<std::size_t>(instruction.operand) >= function.locals.size()) {
                errors_.push_back({"local slot index is out of range in LLVM lowering", {}}); return;
            }
            const std::string p = localPtr(static_cast<std::uint32_t>(instruction.operand));
            if (instruction.opcode == Opcode::LoadLocal) {
                const auto value = temp();
                out_ << "  " << value << " = load %SnValue, ptr " << p << ", align 8\n"
                     << "  store %SnValue " << value << ", ptr " << dst << ", align 8\n";
            } else {
                const auto value = temp();
                out_ << "  " << value << " = load %SnValue, ptr " << stackAt(depth - 1) << ", align 8\n"
                     << "  store %SnValue " << value << ", ptr " << p << ", align 8\n";
            }
            next(); return;
        }
        case Opcode::LoadGlobal:
        case Opcode::StoreGlobal: {
            if (instruction.operand < 0 || static_cast<std::size_t>(instruction.operand) >= module_.globals.size()) {
                errors_.push_back({"global slot index is out of range in LLVM lowering", {}}); return;
            }
            const std::string p = globalPtr(static_cast<std::uint32_t>(instruction.operand));
            if (instruction.opcode == Opcode::LoadGlobal) {
                const auto value = temp();
                out_ << "  " << value << " = load %SnValue, ptr " << p << ", align 8\n"
                     << "  store %SnValue " << value << ", ptr " << dst << ", align 8\n";
            } else {
                const auto value = temp();
                out_ << "  " << value << " = load %SnValue, ptr " << stackAt(depth - 1) << ", align 8\n"
                     << "  store %SnValue " << value << ", ptr " << p << ", align 8\n";
            }
            next(); return;
        }
        case Opcode::Pop:
            next(); return;
        case Opcode::Duplicate: {
            const auto value = temp();
            out_ << "  " << value << " = load %SnValue, ptr " << stackAt(depth - 1) << ", align 8\n"
                 << "  store %SnValue " << value << ", ptr " << dst << ", align 8\n";
            next(); return;
        }
        case Opcode::NewStruct: {
            if (instruction.operand < 0 || static_cast<std::size_t>(instruction.operand) >= module_.structs.size()) {
                errors_.push_back({"NEW_STRUCT references an invalid struct descriptor", {}}); return;
            }
            const auto descriptorIndex = static_cast<std::size_t>(instruction.operand);
            const auto& descriptor = module_.structs[descriptorIndex];
            const auto object = temp();
            const auto raw = temp();
            out_ << "  " << object << " = call ptr @sn_rt_struct_new(i32 " << descriptor.fields.size() << ")\n"
                 << "  " << raw << " = ptrtoint ptr " << object << " to i64\n"
                 << "  call void @sn_rt_make(ptr " << dst << ", i64 " << valueTypeNumber(ValueType::Struct)
                 << ", i64 " << raw << ")\n";
            for (std::size_t fieldIndex = 0; fieldIndex < descriptor.fields.size(); ++fieldIndex) {
                const auto& field = descriptor.fields[fieldIndex];
                if (field.defaultConstant.has_value()) {
                    const std::uint32_t constantIndex = *field.defaultConstant;
                    if (constantIndex >= module_.constants.size()) {
                        errors_.push_back({"struct field default references an invalid constant", {}}); return;
                    }
                    const Constant& constant = module_.constants[constantIndex];
                    if (constant.type == ValueType::String && std::holds_alternative<std::string>(constant.value)) {
                        const auto str = stringPointer(constantIndex);
                        const auto bits = temp();
                        out_ << "  " << bits << " = ptrtoint ptr " << str << " to i64\n"
                             << "  call void @sn_rt_struct_set_value(ptr " << object << ", i32 " << fieldIndex
                             << ", i64 " << valueTypeNumber(constant.type) << ", i64 " << bits << ")\n";
                    } else {
                        out_ << "  call void @sn_rt_struct_set_value(ptr " << object << ", i32 " << fieldIndex
                             << ", i64 " << valueTypeNumber(constant.type) << ", i64 "
                             << constantPayload(constant) << ")\n";
                    }
                } else {
                    out_ << "  call void @sn_rt_struct_default(ptr " << object << ", i32 " << fieldIndex
                         << ", i64 " << valueTypeNumber(field.type.kind) << ")\n";
                }
            }
            next(); return;
        }
        case Opcode::GetField:
            out_ << "  call void @sn_rt_struct_get(ptr " << stackAt(depth - 1)
                 << ", i32 " << instruction.operand << ", ptr " << stackAt(depth - 1) << ")\n";
            next(); return;
        case Opcode::SetField:
            out_ << "  call void @sn_rt_struct_set(ptr " << stackAt(depth - 2)
                 << ", i32 " << instruction.operand << ", ptr " << stackAt(depth - 1) << ")\n";
            next(); return;
        case Opcode::Add: case Opcode::Subtract: case Opcode::Multiply: case Opcode::Divide:
        case Opcode::Modulo: case Opcode::CompareEqual: case Opcode::CompareNotEqual:
        case Opcode::CompareLess: case Opcode::CompareLessEqual: case Opcode::CompareGreater:
        case Opcode::CompareGreaterEqual: case Opcode::BitwiseAnd: case Opcode::BitwiseOr:
        case Opcode::BitwiseXor: case Opcode::ShiftLeft: case Opcode::ShiftRight: {
            out_ << "  call void @sn_rt_binary(ptr " << stackAt(depth - 2) << ", i32 "
                 << static_cast<unsigned int>(instruction.opcode) << ", i32 "
                 << static_cast<unsigned int>(instruction.type) << ", ptr " << stackAt(depth - 2)
                 << ", ptr " << stackAt(depth - 1) << ")\n";
            next(); return;
        }
        case Opcode::Negate:
        case Opcode::LogicalNot:
        case Opcode::BitwiseNot:
            out_ << "  call void @sn_rt_unary(ptr " << stackAt(depth - 1) << ", i32 "
                 << static_cast<unsigned int>(instruction.opcode) << ", i32 "
                 << static_cast<unsigned int>(instruction.type) << ", ptr " << stackAt(depth - 1) << ")\n";
            next(); return;
        case Opcode::Jump:
            if (instruction.operand < 0 || static_cast<std::size_t>(instruction.operand) > function.code.size()) {
                errors_.push_back({"JUMP target is outside the function", {}}); return;
            }
            out_ << "  br label %bb" << instruction.operand << "\n\n";
            return;
        case Opcode::JumpIfFalse: {
            const auto payloadPtr = temp();
            const auto payload = temp();
            const auto isFalse = temp();
            out_ << "  " << payloadPtr << " = getelementptr inbounds %SnValue, ptr "
                 << stackAt(depth - 1) << ", i32 0, i32 1\n"
                 << "  " << payload << " = load i64, ptr " << payloadPtr << ", align 8\n"
                 << "  " << isFalse << " = icmp eq i64 " << payload << ", 0\n"
                 << "  br i1 " << isFalse << ", label %bb" << instruction.operand
                 << ", label %bb" << (pc + 1) << "\n\n";
            return;
        }
        case Opcode::Call: {
            if (instruction.operand < 0 || static_cast<std::size_t>(instruction.operand) >= module_.functions.size() || instruction.extra < 0) {
                errors_.push_back({"CALL has invalid function or argument index", {}}); return;
            }
            const std::size_t argc = static_cast<std::size_t>(instruction.extra);
            const std::int64_t base = depth - static_cast<std::int64_t>(argc);
            for (std::size_t index = 0; index < argc; ++index) {
                const auto argPtr = temp();
                const auto value = temp();
                out_ << "  " << argPtr << " = getelementptr inbounds [" << std::max<std::size_t>(1, argc)
                     << " x %SnValue], ptr %call_args_" << pc << ", i32 0, i32 " << index << '\n'
                     << "  " << value << " = load %SnValue, ptr "
                     << stackAt(base + static_cast<std::int64_t>(index)) << ", align 8\n"
                     << "  store %SnValue " << value << ", ptr " << argPtr << ", align 8\n";
            }
            const std::string args = callArgsPointer(pc, argc);
            out_ << "  call void @sn_fn_" << instruction.operand << "(ptr " << args
                 << ", ptr %call_out_" << pc << ")\n";
            if (module_.functions[static_cast<std::size_t>(instruction.operand)].returnType.kind != ValueType::Void) {
                const auto value = temp();
                out_ << "  " << value << " = load %SnValue, ptr %call_out_" << pc << ", align 8\n"
                     << "  store %SnValue " << value << ", ptr " << stackAt(base) << ", align 8\n";
            }
            next(); return;
        }
        case Opcode::Return:
            if (function.returnType.kind != ValueType::Void) {
                const auto value = temp();
                out_ << "  " << value << " = load %SnValue, ptr " << stackAt(depth - 1) << ", align 8\n"
                     << "  store %SnValue " << value << ", ptr %out, align 8\n";
            }
            out_ << "  ret void\n\n";
            return;
        case Opcode::Print:
        case Opcode::Println:
            out_ << "  call void @sn_rt_print(ptr " << stackAt(depth - 1) << ", i1 "
                 << (instruction.opcode == Opcode::Println ? "true" : "false") << ")\n";
            next(); return;
        }
    }

    void emitNativeMain() {
        const std::uint32_t entryIndex = *module_.entryFunction;
        out_ << "define i32 @main() {\nentry:\n"
             << "  %entry_args = alloca %SnValue, align 8\n"
             << "  %entry_out = alloca %SnValue, align 8\n"
             << "  call void @sn_rt_default(ptr %entry_out, i64 "
             << valueTypeNumber(ValueType::Null) << ")\n";
        if (module_.globalInitializerFunction.has_value()) {
            out_ << "  call void @sn_fn_" << *module_.globalInitializerFunction
                 << "(ptr %entry_args, ptr %entry_out)\n";
        }
        out_ << "  call void @sn_fn_" << entryIndex << "(ptr %entry_args, ptr %entry_out)\n"
             << "  %exit_code = call i32 @sn_rt_exit_code(ptr %entry_out)\n"
             << "  ret i32 %exit_code\n}\n";
    }
};

} // namespace

LLVMIREmitter::LLVMIREmitter(const bytecode::BytecodeModule& module) : module_(module) {}

LLVMResult LLVMIREmitter::emit() const {
    return Emitter(module_).run();
}

} // namespace serenity::aot
