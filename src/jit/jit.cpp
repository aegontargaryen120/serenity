#include "serenity/jit/jit.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if SERENITY_HAS_LLVM
#include <llvm/AsmParser/Parser.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Config/llvm-config.h>
#endif

namespace serenity::jit {
namespace {

using bytecode::BytecodeModule;
using bytecode::Function;
using bytecode::Instruction;
using bytecode::Opcode;
using bytecode::ValueType;
using interpreter::Value;

constexpr std::size_t InvalidPc = std::numeric_limits<std::size_t>::max();

enum class TraceExit : std::uint32_t {
    SideExit = 0,
    Budget = 1,
    Deoptimization = 2,
};

struct LoopKey {
    std::uint32_t functionIndex {0};
    std::size_t header {0};

    friend bool operator==(const LoopKey&, const LoopKey&) = default;
};

struct LoopKeyHash {
    std::size_t operator()(const LoopKey& key) const noexcept {
        const std::size_t a = std::hash<std::uint32_t>{}(key.functionIndex);
        const std::size_t b = std::hash<std::size_t>{}(key.header);
        return a ^ (b + 0x9e3779b9U + (a << 6U) + (a >> 2U));
    }
};

struct TraceEvent {
    std::size_t pc {0};
    Instruction instruction;
    std::optional<bool> branchCondition;
    std::size_t nextPc {InvalidPc};
    std::size_t stackDepthAfter {InvalidPc};
};

struct SlotAccess {
    bool global {false};
    std::uint32_t index {0};
    ValueType type {ValueType::Invalid};
};

using NativeTraceFunction = std::uint32_t (*)(
    std::int64_t*, std::int64_t*, std::uint8_t*, std::uint32_t*, std::uint32_t*,
    std::uint64_t*, std::uint64_t*, std::uint64_t);

struct CompiledTrace {
    LoopKey key;
    NativeTraceFunction function {nullptr};
    std::vector<SlotAccess> slots;
    std::size_t maxStackDepth {0};
    std::string symbol;
};

[[nodiscard]] bool isTraceableSlotType(ValueType type) noexcept {
    return type == ValueType::Int || type == ValueType::Bool;
}

[[nodiscard]] std::optional<std::int64_t> integerConstant(const bytecode::Constant& constant) {
    if (const auto* value = std::get_if<std::int64_t>(&constant.value)) return *value;
    return std::nullopt;
}

[[nodiscard]] std::optional<bool> booleanConstant(const bytecode::Constant& constant) {
    if (const auto* value = std::get_if<bool>(&constant.value)) return *value;
    return std::nullopt;
}

struct StackValue {
    std::string ir;
    ValueType type {ValueType::Invalid};
};

class TraceIRBuilder {
public:
    TraceIRBuilder(const BytecodeModule& module, const Function& function,
                   LoopKey key, const std::vector<TraceEvent>& events,
                   std::string symbol)
        : module_(module), function_(function), key_(key), events_(events),
          symbol_(std::move(symbol)) {}

    [[nodiscard]] std::optional<std::string> build(std::string& error) {
        if (!validate(error)) return std::nullopt;

        out_ << "; Serenity tracing JIT trace for function " << function_.name
             << " at bytecode PC " << key_.header << "\n"
             << "define i32 @" << symbol_
             << "(ptr %slots, ptr %stack_values, ptr %stack_types, "
                "ptr %exit_pc, ptr %exit_depth, ptr %executed, "
                "ptr %iterations, i64 %budget) {\n"
             << "entry:\n  br label %trace_start\n"
             << "trace_start:\n"
             << "  %iter.old = load i64, ptr %iterations, align 8\n"
             << "  %iter.new = add i64 %iter.old, 1\n"
             << "  store i64 %iter.new, ptr %iterations, align 8\n"
             << "  br label %b0\n";

        stack_.clear();
        for (std::size_t index = 0; index < events_.size(); ++index) {
            if (!emitEvent(index, events_[index], error)) return std::nullopt;
        }
        out_ << "}\n";
        return out_.str();
    }

    [[nodiscard]] const std::vector<SlotAccess>& slots() const noexcept { return touchedSlots_; }
    [[nodiscard]] std::size_t maxStackDepth() const noexcept { return maxStackDepth_; }

private:
    const BytecodeModule& module_;
    const Function& function_;
    LoopKey key_;
    const std::vector<TraceEvent>& events_;
    std::string symbol_;
    std::ostringstream out_;
    std::vector<StackValue> stack_;
    std::vector<SlotAccess> touchedSlots_;
    std::unordered_set<std::uint64_t> touchedSlotKeys_;
    std::size_t temp_ {0};
    std::size_t maxStackDepth_ {0};

    [[nodiscard]] std::string temp() { return "%v" + std::to_string(temp_++); }

    [[nodiscard]] std::optional<SlotAccess> slotAccess(const Instruction& instruction,
                                                       bool global, std::string& error) {
        if (instruction.operand < 0) {
            error = "trace contains a negative local/global slot index";
            return std::nullopt;
        }
        const std::size_t index = static_cast<std::size_t>(instruction.operand);
        ValueType type = ValueType::Invalid;
        if (global) {
            if (index >= module_.globals.size()) {
                error = "trace contains an out-of-range global slot";
                return std::nullopt;
            }
            type = module_.globals[index].type.kind;
        } else {
            if (index >= function_.locals.size()) {
                error = "trace contains an out-of-range local slot";
                return std::nullopt;
            }
            type = function_.locals[index].type.kind;
        }
        if (!isTraceableSlotType(type)) {
            error = "trace uses a slot whose type is not int or bool";
            return std::nullopt;
        }
        SlotAccess access{global, static_cast<std::uint32_t>(index), type};
        const std::uint64_t key = (global ? (std::uint64_t{1} << 63U) : 0U) | index;
        if (touchedSlotKeys_.insert(key).second) touchedSlots_.push_back(access);
        return access;
    }

    [[nodiscard]] bool validate(std::string& error) {
        if (events_.size() < 2) {
            error = "trace is too short to compile";
            return false;
        }
        if (events_.front().pc != key_.header) {
            error = "trace does not begin at its loop header";
            return false;
        }
        const TraceEvent& terminal = events_.back();
        if (terminal.instruction.opcode != Opcode::Jump || terminal.nextPc != key_.header ||
            terminal.instruction.operand < 0 ||
            static_cast<std::size_t>(terminal.instruction.operand) != key_.header) {
            error = "trace does not end at its loop backedge";
            return false;
        }
        for (std::size_t index = 0; index + 1 < events_.size(); ++index) {
            if (events_[index].nextPc != events_[index + 1].pc) {
                error = "recorded trace contains a discontinuous bytecode path";
                return false;
            }
        }
        return true;
    }

    void emitIncrement() {
        const std::string oldCount = temp();
        const std::string newCount = temp();
        out_ << "  " << oldCount << " = load i64, ptr %executed, align 8\n"
             << "  " << newCount << " = add i64 " << oldCount << ", 1\n"
             << "  store i64 " << newCount << ", ptr %executed, align 8\n";
    }

    [[nodiscard]] std::vector<StackValue> stackWithPoppedValues() const {
        std::vector<StackValue> values = stack_;
        values.insert(values.end(), popped_.begin(), popped_.end());
        return values;
    }

    void emitExit(std::size_t pc, const std::vector<StackValue>& values,
                  TraceExit kind, const std::string& label) {
        maxStackDepth_ = std::max(maxStackDepth_, values.size());
        out_ << label << ":\n"
             << "  store i32 " << pc << ", ptr %exit_pc, align 4\n"
             << "  store i32 " << values.size() << ", ptr %exit_depth, align 4\n";
        for (std::size_t index = 0; index < values.size(); ++index) {
            std::string payload = values[index].ir;
            if (values[index].type == ValueType::Bool) {
                const std::string widened = temp();
                out_ << "  " << widened << " = zext i1 " << payload << " to i64\n";
                payload = widened;
            }
            const std::string valuePtr = temp();
            const std::string typePtr = temp();
            out_ << "  " << valuePtr << " = getelementptr i64, ptr %stack_values, i64 " << index << "\n"
                 << "  store i64 " << payload << ", ptr " << valuePtr << ", align 8\n"
                 << "  " << typePtr << " = getelementptr i8, ptr %stack_types, i64 " << index << "\n"
                 << "  store i8 " << static_cast<unsigned int>(values[index].type)
                 << ", ptr " << typePtr << ", align 1\n";
        }
        out_ << "  ret i32 " << static_cast<unsigned int>(kind) << "\n";
    }

    [[nodiscard]] bool pop(std::size_t count, std::vector<StackValue>& values, std::string& error) {
        maxStackDepth_ = std::max(maxStackDepth_, stack_.size());
        if (stack_.size() < count) {
            error = "trace stack underflow while lowering bytecode";
            return false;
        }
        values.clear();
        values.reserve(count);
        const std::size_t start = stack_.size() - count;
        for (std::size_t index = start; index < stack_.size(); ++index) values.push_back(stack_[index]);
        stack_.resize(start);
        return true;
    }

    [[nodiscard]] bool emitEvent(std::size_t eventIndex, const TraceEvent& event, std::string& error) {
        const Instruction& instruction = event.instruction;
        out_ << "b" << eventIndex << ":\n";
        const std::string count = temp();
        const std::string outOfBudget = temp();
        out_ << "  " << count << " = load i64, ptr %executed, align 8\n"
             << "  " << outOfBudget << " = icmp uge i64 " << count << ", %budget\n"
             << "  br i1 " << outOfBudget << ", label %budget_exit_" << eventIndex
             << ", label %run_" << eventIndex << "\n";
        emitExit(event.pc, stack_, TraceExit::Budget, "budget_exit_" + std::to_string(eventIndex));
        out_ << "run_" << eventIndex << ":\n";

        const auto branchNext = [&]() {
            if (eventIndex + 1 == events_.size()) out_ << "  br label %trace_start\n";
            else out_ << "  br label %b" << (eventIndex + 1) << "\n";
        };

        switch (instruction.opcode) {
        case Opcode::Nop:
            emitIncrement();
            branchNext();
            break;

        case Opcode::PushConstant: {
            if (instruction.operand < 0 ||
                static_cast<std::size_t>(instruction.operand) >= module_.constants.size()) {
                error = "trace contains an invalid constant-pool index";
                return false;
            }
            const auto& constant = module_.constants[static_cast<std::size_t>(instruction.operand)];
            if (const auto intValue = integerConstant(constant); intValue.has_value()) {
                emitIncrement();
                stack_.push_back(StackValue{std::to_string(*intValue), ValueType::Int});
            } else if (const auto boolValue = booleanConstant(constant); boolValue.has_value()) {
                emitIncrement();
                stack_.push_back(StackValue{*boolValue ? "true" : "false", ValueType::Bool});
            } else {
                error = "trace supports only integer and boolean constants";
                return false;
            }
            branchNext();
            break;
        }

        case Opcode::LoadLocal:
        case Opcode::LoadGlobal: {
            const bool global = instruction.opcode == Opcode::LoadGlobal;
            const auto access = slotAccess(instruction, global, error);
            if (!access.has_value()) return false;
            emitIncrement();
            const std::size_t slot = global ? function_.locals.size() + access->index : access->index;
            const std::string pointer = temp();
            const std::string loaded = temp();
            out_ << "  " << pointer << " = getelementptr i64, ptr %slots, i64 " << slot << "\n"
                 << "  " << loaded << " = load i64, ptr " << pointer << ", align 8\n";
            if (access->type == ValueType::Bool) {
                const std::string boolean = temp();
                out_ << "  " << boolean << " = icmp ne i64 " << loaded << ", 0\n";
                stack_.push_back(StackValue{boolean, ValueType::Bool});
            } else {
                stack_.push_back(StackValue{loaded, ValueType::Int});
            }
            branchNext();
            break;
        }

        case Opcode::StoreLocal:
        case Opcode::StoreGlobal: {
            const bool global = instruction.opcode == Opcode::StoreGlobal;
            const auto access = slotAccess(instruction, global, error);
            if (!access.has_value()) return false;
            if (stack_.empty() || stack_.back().type != access->type) {
                error = "trace store has a value with the wrong static type";
                return false;
            }
            const StackValue value = stack_.back();
            stack_.pop_back();
            emitIncrement();
            const std::size_t slot = global ? function_.locals.size() + access->index : access->index;
            const std::string pointer = temp();
            std::string payload = value.ir;
            if (value.type == ValueType::Bool) {
                const std::string widened = temp();
                out_ << "  " << widened << " = zext i1 " << payload << " to i64\n";
                payload = widened;
            }
            out_ << "  " << pointer << " = getelementptr i64, ptr %slots, i64 " << slot << "\n"
                 << "  store i64 " << payload << ", ptr " << pointer << ", align 8\n";
            branchNext();
            break;
        }

        case Opcode::Duplicate:
            if (stack_.empty()) { error = "DUP on an empty trace stack"; return false; }
            emitIncrement();
            stack_.push_back(stack_.back());
            branchNext();
            break;

        case Opcode::Pop:
            if (!pop(1, popped_, error)) return false;
            emitIncrement();
            branchNext();
            break;

        case Opcode::Add:
        case Opcode::Subtract:
        case Opcode::Multiply:
        case Opcode::Divide:
        case Opcode::Modulo: {
            if (instruction.type != ValueType::Int || !pop(2, popped_, error)) {
                if (error.empty()) error = "traced arithmetic currently requires int operands";
                return false;
            }
            if (popped_[0].type != ValueType::Int || popped_[1].type != ValueType::Int) {
                error = "traced arithmetic received non-integer values";
                return false;
            }
            const std::string result = temp();
            if (instruction.opcode == Opcode::Add || instruction.opcode == Opcode::Subtract ||
                instruction.opcode == Opcode::Multiply) {
                const char* intrinsic = instruction.opcode == Opcode::Add ? "sadd" :
                    (instruction.opcode == Opcode::Subtract ? "ssub" : "smul");
                const std::string pair = temp();
                const std::string overflow = temp();
                out_ << "  " << pair << " = call { i64, i1 } @llvm." << intrinsic
                     << ".with.overflow.i64(i64 " << popped_[0].ir << ", i64 " << popped_[1].ir << ")\n"
                     << "  " << result << " = extractvalue { i64, i1 } " << pair << ", 0\n"
                     << "  " << overflow << " = extractvalue { i64, i1 } " << pair << ", 1\n"
                     << "  br i1 " << overflow << ", label %deopt_" << eventIndex
                     << ", label %checked_" << eventIndex << "\n";
                emitExit(event.pc, stackWithPoppedValues(), TraceExit::Deoptimization, "deopt_" + std::to_string(eventIndex));
                out_ << "checked_" << eventIndex << ":\n";
            } else {
                const std::string zero = temp();
                const std::string minimum = temp();
                const std::string minusOne = temp();
                const std::string overflow = temp();
                const std::string bad = temp();
                out_ << "  " << zero << " = icmp eq i64 " << popped_[1].ir << ", 0\n"
                     << "  " << minimum << " = icmp eq i64 " << popped_[0].ir << ", -9223372036854775808\n"
                     << "  " << minusOne << " = icmp eq i64 " << popped_[1].ir << ", -1\n"
                     << "  " << overflow << " = and i1 " << minimum << ", " << minusOne << "\n"
                     << "  " << bad << " = or i1 " << zero << ", " << overflow << "\n"
                     << "  br i1 " << bad << ", label %deopt_" << eventIndex
                     << ", label %checked_" << eventIndex << "\n";
                emitExit(event.pc, stackWithPoppedValues(), TraceExit::Deoptimization, "deopt_" + std::to_string(eventIndex));
                out_ << "checked_" << eventIndex << ":\n";
            }
            emitIncrement();
            const char* operation = instruction.opcode == Opcode::Divide ? "sdiv" :
                (instruction.opcode == Opcode::Modulo ? "srem" : nullptr);
            if (operation != nullptr) {
                out_ << "  " << result << " = " << operation << " i64 "
                     << popped_[0].ir << ", " << popped_[1].ir << "\n";
            }
            stack_.push_back(StackValue{result, ValueType::Int});
            branchNext();
            break;
        }

        case Opcode::Negate: {
            if (instruction.type != ValueType::Int || !pop(1, popped_, error)) {
                if (error.empty()) error = "traced negation currently requires int";
                return false;
            }
            if (popped_[0].type != ValueType::Int) { error = "NEG received non-int in trace"; return false; }
            const std::string bad = temp();
            const std::string result = temp();
            out_ << "  " << bad << " = icmp eq i64 " << popped_[0].ir << ", -9223372036854775808\n"
                 << "  br i1 " << bad << ", label %deopt_" << eventIndex
                 << ", label %checked_" << eventIndex << "\n";
            emitExit(event.pc, stackWithPoppedValues(), TraceExit::Deoptimization, "deopt_" + std::to_string(eventIndex));
            out_ << "checked_" << eventIndex << ":\n";
            emitIncrement();
            out_ << "  " << result << " = sub i64 0, " << popped_[0].ir << "\n";
            stack_.push_back(StackValue{result, ValueType::Int});
            branchNext();
            break;
        }

        case Opcode::LogicalNot: {
            if (!pop(1, popped_, error)) return false;
            if (popped_[0].type != ValueType::Bool) { error = "logical NOT expected bool"; return false; }
            emitIncrement();
            const std::string result = temp();
            out_ << "  " << result << " = xor i1 " << popped_[0].ir << ", true\n";
            stack_.push_back(StackValue{result, ValueType::Bool});
            branchNext();
            break;
        }

        case Opcode::BitwiseNot: {
            if (!pop(1, popped_, error)) return false;
            if (popped_[0].type != ValueType::Int) { error = "bitwise NOT expected int"; return false; }
            emitIncrement();
            const std::string result = temp();
            out_ << "  " << result << " = xor i64 " << popped_[0].ir << ", -1\n";
            stack_.push_back(StackValue{result, ValueType::Int});
            branchNext();
            break;
        }

        case Opcode::CompareEqual:
        case Opcode::CompareNotEqual:
        case Opcode::CompareLess:
        case Opcode::CompareLessEqual:
        case Opcode::CompareGreater:
        case Opcode::CompareGreaterEqual: {
            if (!pop(2, popped_, error)) return false;
            if (popped_[0].type != instruction.type || popped_[1].type != instruction.type ||
                (instruction.type != ValueType::Int && instruction.type != ValueType::Bool)) {
                error = "trace comparison requires matching int or bool operands";
                return false;
            }
            emitIncrement();
            const std::string result = temp();
            const char* predicate = nullptr;
            if (instruction.type == ValueType::Int) {
                switch (instruction.opcode) {
                case Opcode::CompareEqual: predicate = "eq"; break;
                case Opcode::CompareNotEqual: predicate = "ne"; break;
                case Opcode::CompareLess: predicate = "slt"; break;
                case Opcode::CompareLessEqual: predicate = "sle"; break;
                case Opcode::CompareGreater: predicate = "sgt"; break;
                case Opcode::CompareGreaterEqual: predicate = "sge"; break;
                default: break;
                }
            } else {
                switch (instruction.opcode) {
                case Opcode::CompareEqual: predicate = "eq"; break;
                case Opcode::CompareNotEqual: predicate = "ne"; break;
                case Opcode::CompareLess: predicate = "ult"; break;
                case Opcode::CompareLessEqual: predicate = "ule"; break;
                case Opcode::CompareGreater: predicate = "ugt"; break;
                case Opcode::CompareGreaterEqual: predicate = "uge"; break;
                default: break;
                }
            }
            const char* llvmType = instruction.type == ValueType::Bool ? "i1" : "i64";
            out_ << "  " << result << " = icmp " << predicate << ' ' << llvmType << ' '
                 << popped_[0].ir << ", " << popped_[1].ir << "\n";
            stack_.push_back(StackValue{result, ValueType::Bool});
            branchNext();
            break;
        }

        case Opcode::BitwiseAnd:
        case Opcode::BitwiseOr:
        case Opcode::BitwiseXor: {
            if (!pop(2, popped_, error)) return false;
            if (popped_[0].type != ValueType::Int || popped_[1].type != ValueType::Int) {
                error = "bitwise operations require int operands";
                return false;
            }
            emitIncrement();
            const std::string result = temp();
            const char* op = instruction.opcode == Opcode::BitwiseAnd ? "and" :
                (instruction.opcode == Opcode::BitwiseOr ? "or" : "xor");
            out_ << "  " << result << " = " << op << " i64 " << popped_[0].ir
                 << ", " << popped_[1].ir << "\n";
            stack_.push_back(StackValue{result, ValueType::Int});
            branchNext();
            break;
        }

        case Opcode::ShiftLeft:
        case Opcode::ShiftRight: {
            if (!pop(2, popped_, error)) return false;
            if (popped_[0].type != ValueType::Int || popped_[1].type != ValueType::Int) {
                error = "shift operations require int operands";
                return false;
            }
            const std::string negative = temp();
            const std::string tooLarge = temp();
            const std::string badCount = temp();
            out_ << "  " << negative << " = icmp slt i64 " << popped_[1].ir << ", 0\n"
                 << "  " << tooLarge << " = icmp sge i64 " << popped_[1].ir << ", 63\n"
                 << "  " << badCount << " = or i1 " << negative << ", " << tooLarge << "\n";
            if (instruction.opcode == Opcode::ShiftLeft) {
                const std::string negativeLeft = temp();
                const std::string badBase = temp();
                const std::string bad = temp();
                out_ << "  " << negativeLeft << " = icmp slt i64 " << popped_[0].ir << ", 0\n"
                     << "  " << badBase << " = or i1 " << badCount << ", " << negativeLeft << "\n"
                     << "  br i1 " << badBase << ", label %deopt_" << eventIndex
                     << ", label %shift_check_" << eventIndex << "\n";
                emitExit(event.pc, stackWithPoppedValues(), TraceExit::Deoptimization, "deopt_" + std::to_string(eventIndex));
                out_ << "shift_check_" << eventIndex << ":\n";
                const std::string limit = temp();
                const std::string overflow = temp();
                out_ << "  " << limit << " = lshr i64 9223372036854775807, " << popped_[1].ir << "\n"
                     << "  " << overflow << " = icmp ugt i64 " << popped_[0].ir << ", " << limit << "\n"
                     << "  br i1 " << overflow << ", label %deopt_shift_" << eventIndex
                     << ", label %shift_ok_" << eventIndex << "\n";
                emitExit(event.pc, stackWithPoppedValues(), TraceExit::Deoptimization, "deopt_shift_" + std::to_string(eventIndex));
            } else {
                out_ << "  br i1 " << badCount << ", label %deopt_" << eventIndex
                     << ", label %shift_ok_" << eventIndex << "\n";
                emitExit(event.pc, stackWithPoppedValues(), TraceExit::Deoptimization, "deopt_" + std::to_string(eventIndex));
            }
            out_ << "shift_ok_" << eventIndex << ":\n";
            emitIncrement();
            const std::string result = temp();
            out_ << "  " << result << " = "
                 << (instruction.opcode == Opcode::ShiftLeft ? "shl" : "ashr")
                 << " i64 " << popped_[0].ir << ", " << popped_[1].ir << "\n";
            stack_.push_back(StackValue{result, ValueType::Int});
            branchNext();
            break;
        }

        case Opcode::JumpIfFalse: {
            if (!pop(1, popped_, error)) return false;
            if (popped_[0].type != ValueType::Bool || !event.branchCondition.has_value()) {
                error = "recorded conditional branch is missing a boolean outcome";
                return false;
            }
            const std::size_t target = instruction.operand < 0
                ? InvalidPc : static_cast<std::size_t>(instruction.operand);
            const std::size_t fallthrough = event.pc + 1;
            const bool expected = *event.branchCondition;
            const std::size_t expectedNext = expected ? fallthrough : target;
            const std::size_t alternate = expected ? target : fallthrough;
            if (event.nextPc != expectedNext || target == InvalidPc) {
                error = "recorded conditional branch has an inconsistent target";
                return false;
            }
            emitIncrement();
            const std::string branchMatches = temp();
            out_ << "  " << branchMatches << " = icmp eq i1 " << popped_[0].ir
                 << ", " << (expected ? "true" : "false") << "\n"
                 << "  br i1 " << branchMatches << ", label %b" << (eventIndex + 1)
                 << ", label %side_exit_" << eventIndex << "\n";
            emitExit(alternate, stack_, TraceExit::SideExit, "side_exit_" + std::to_string(eventIndex));
            break;
        }

        case Opcode::Jump: {
            if (instruction.operand < 0) { error = "trace contains a negative jump target"; return false; }
            const std::size_t target = static_cast<std::size_t>(instruction.operand);
            emitIncrement();
            if (target == key_.header && eventIndex + 1 == events_.size()) {
                if (!stack_.empty()) { error = "trace backedge has a non-empty operand stack"; return false; }
                out_ << "  br label %trace_start\n";
            } else {
                if (event.nextPc != target || eventIndex + 1 >= events_.size()) {
                    error = "trace contains an unsupported non-terminal jump";
                    return false;
                }
                branchNext();
            }
            break;
        }

        default:
            error = "trace contains an unsupported opcode: " + std::string(bytecode::opcodeName(instruction.opcode));
            return false;
        }

        maxStackDepth_ = std::max(maxStackDepth_, stack_.size());
        if (stack_.size() != event.stackDepthAfter) {
            error = "recorded operand-stack depth disagrees with trace simulation";
            return false;
        }
        if (eventIndex + 1 == events_.size() && !stack_.empty()) {
            error = "trace ended with a non-empty operand stack";
            return false;
        }
        return true;
    }

    std::vector<StackValue> popped_;
};

} // namespace

class TracingJIT::Impl final : public interpreter::InterpreterHooks {
public:
    Impl(const BytecodeModule& module, JitOptions options)
        : module_(module), options_(options) {
#if SERENITY_HAS_LLVM
        if (llvm::InitializeNativeTarget()) {
            initializationError_ = "LLVM could not initialize the native target";
            return;
        }
        if (llvm::InitializeNativeTargetAsmPrinter()) {
            initializationError_ = "LLVM could not initialize the native assembly printer";
            return;
        }
        if (llvm::InitializeNativeTargetAsmParser()) {
            initializationError_ = "LLVM could not initialize the native assembly parser";
            return;
        }
        auto created = llvm::orc::LLJITBuilder().create();
        if (!created) {
            initializationError_ = llvm::toString(created.takeError());
            return;
        }
        llvmJit_ = std::move(*created);
        available_ = true;
#else
        initializationError_ =
            "snjit was built without LLVM ORC development libraries; install Homebrew LLVM and reconfigure CMake";
#endif
    }

    [[nodiscard]] JitResult run(std::ostream& output) {
        JitResult result;
        result.jitAvailable = available_;
        result.initializationError = initializationError_;
        if (!available_) {
            result.execution.ok = false;
            result.execution.error = interpreter::RuntimeError{
                initializationError_, 0, "<jit>", 0, {}};
            result.jit = statistics_;
            return result;
        }

        interpreter::InterpreterOptions interpreterOptions;
        interpreterOptions.maxInstructions = options_.maxInstructions;
        interpreterOptions.maxCallDepth = options_.maxCallDepth;
        interpreterOptions.hooks = this;
        interpreter::Interpreter vm(module_, output, interpreterOptions);
        result.execution = vm.run();
        result.jit = statistics_;
        return result;
    }

    [[nodiscard]] bool available() const noexcept { return available_; }
    [[nodiscard]] const std::string& initializationError() const noexcept { return initializationError_; }

    bool tryExecuteTrace(
        std::uint32_t functionIndex,
        std::size_t instructionPointer,
        const Function& function,
        std::vector<Value>& locals,
        std::vector<Value>& globals,
        std::vector<Value>& operandStack,
        std::size_t stackBase,
        std::size_t& nextInstructionPointer,
        std::uint64_t instructionBudget,
        std::uint64_t& instructionsExecuted) override {
        const auto found = traces_.find(LoopKey{functionIndex, instructionPointer});
        if (found == traces_.end() || operandStack.size() != stackBase) return false;
        const CompiledTrace& trace = found->second;

        const std::size_t totalSlots = function.locals.size() + module_.globals.size();
        std::vector<std::int64_t> slots(std::max<std::size_t>(1, totalSlots), 0);
        for (const SlotAccess& access : trace.slots) {
            const Value& value = access.global ? globals[access.index] : locals[access.index];
            const std::size_t slotIndex = access.global ? function.locals.size() + access.index : access.index;
            if (access.type == ValueType::Int) {
                const auto* integer = std::get_if<std::int64_t>(&value.data);
                if (integer == nullptr) { ++statistics_.deoptimizations; return false; }
                slots[slotIndex] = *integer;
            } else {
                const auto* boolean = std::get_if<bool>(&value.data);
                if (boolean == nullptr) { ++statistics_.deoptimizations; return false; }
                slots[slotIndex] = *boolean ? 1 : 0;
            }
        }

        std::vector<std::int64_t> deoptValues(std::max<std::size_t>(1, trace.maxStackDepth), 0);
        std::vector<std::uint8_t> deoptTypes(std::max<std::size_t>(1, trace.maxStackDepth), 0);
        std::uint32_t exitPc = static_cast<std::uint32_t>(instructionPointer);
        std::uint32_t exitDepth = 0;
        std::uint64_t executed = 0;
        std::uint64_t iterations = 0;
        const std::uint32_t exitKind = trace.function(
            slots.data(), deoptValues.data(), deoptTypes.data(), &exitPc, &exitDepth,
            &executed, &iterations, instructionBudget);
        if (exitDepth > trace.maxStackDepth || exitPc > function.code.size() ||
            (exitKind != static_cast<std::uint32_t>(TraceExit::SideExit) &&
             exitKind != static_cast<std::uint32_t>(TraceExit::Budget) &&
             exitKind != static_cast<std::uint32_t>(TraceExit::Deoptimization))) {
            ++statistics_.deoptimizations;
            return false;
        }
        for (std::uint32_t index = 0; index < exitDepth; ++index) {
            const ValueType type = static_cast<ValueType>(deoptTypes[index]);
            if (type != ValueType::Int && type != ValueType::Bool) {
                ++statistics_.deoptimizations;
                return false;
            }
        }

        for (const SlotAccess& access : trace.slots) {
            const std::size_t slotIndex = access.global ? function.locals.size() + access.index : access.index;
            Value updated;
            if (access.type == ValueType::Bool) {
                updated.data.emplace<bool>(slots[slotIndex] != 0);
            } else {
                updated.data.emplace<std::int64_t>(slots[slotIndex]);
            }
            if (access.global) globals[access.index] = std::move(updated);
            else locals[access.index] = std::move(updated);
        }

        operandStack.resize(stackBase);
        for (std::uint32_t index = 0; index < exitDepth; ++index) {
            const ValueType type = static_cast<ValueType>(deoptTypes[index]);
            operandStack.emplace_back();
            Value& restored = operandStack.back();
            if (type == ValueType::Int) {
                restored.data.emplace<std::int64_t>(deoptValues[index]);
            } else {
                // The exit tags were validated above; only bool remains here.
                restored.data.emplace<bool>(deoptValues[index] != 0);
            }
        }

        nextInstructionPointer = exitPc;
        instructionsExecuted = executed;
        ++statistics_.traceExecutions;
        statistics_.traceIterations += iterations;
        statistics_.nativeBytecodeInstructions += executed;
        if (exitKind == static_cast<std::uint32_t>(TraceExit::Budget)) {
            ++statistics_.budgetExits;
        } else if (exitKind == static_cast<std::uint32_t>(TraceExit::Deoptimization)) {
            ++statistics_.deoptimizations;
        } else {
            ++statistics_.sideExits;
        }
        return true;
    }

    void onInstructionExecuted(
        std::uint32_t functionIndex,
        std::size_t instructionPointer,
        const Instruction& instruction,
        std::optional<bool> branchCondition,
        std::size_t nextInstructionPointer,
        std::size_t relativeStackDepth) override {
        const std::size_t backedgeTarget = instruction.opcode == Opcode::Jump && instruction.operand >= 0
            ? static_cast<std::size_t>(instruction.operand) : InvalidPc;
        const bool isBackedge = backedgeTarget != InvalidPc && backedgeTarget <= instructionPointer &&
                                nextInstructionPointer == backedgeTarget;

        if (recording_.has_value()) {
            Recording& recording = *recording_;
            if (recording.key.functionIndex != functionIndex ||
                nextInstructionPointer == InvalidPc || relativeStackDepth == InvalidPc) {
                rejectRecording();
                return;
            }
            recording.events.push_back(TraceEvent{
                instructionPointer, instruction, branchCondition,
                nextInstructionPointer, relativeStackDepth});
            if (recording.events.size() > 2048) {
                rejectRecording();
                return;
            }
            if (isBackedge && backedgeTarget == recording.key.header) {
                finishRecording();
                return;
            }
            if (isBackedge && backedgeTarget != recording.key.header) {
                rejectRecording();
                return;
            }
            return;
        }

        if (!isBackedge) return;
        ++statistics_.observedBackedges;
        const LoopKey key{functionIndex, backedgeTarget};
        if (traces_.contains(key) || attempted_.contains(key)) return;
        const std::uint64_t count = ++backedgeCounts_[key];
        if (count >= std::max<std::uint64_t>(1, options_.hotLoopThreshold)) {
            attempted_.insert(key);
            ++statistics_.recordingAttempts;
            recording_ = Recording{key, {}};
        }
    }

private:
    struct Recording {
        LoopKey key;
        std::vector<TraceEvent> events;
    };

    const BytecodeModule& module_;
    JitOptions options_;
    JitStatistics statistics_;
    bool available_ {false};
    std::string initializationError_;
    std::optional<Recording> recording_;
    std::unordered_map<LoopKey, std::uint64_t, LoopKeyHash> backedgeCounts_;
    std::unordered_set<LoopKey, LoopKeyHash> attempted_;
    std::unordered_map<LoopKey, CompiledTrace, LoopKeyHash> traces_;
#if SERENITY_HAS_LLVM
    std::unique_ptr<llvm::orc::LLJIT> llvmJit_;
    std::uint64_t nextTraceId_ {0};
#endif

    void rejectRecording() {
        if (recording_.has_value()) {
            ++statistics_.rejectedTraces;
            recording_.reset();
        }
    }

    void finishRecording() {
        if (!recording_.has_value()) return;
        Recording recording = std::move(*recording_);
        recording_.reset();
#if SERENITY_HAS_LLVM
        const auto started = std::chrono::steady_clock::now();
        const std::string symbol = "serenity_trace_" + std::to_string(nextTraceId_++);
        if (recording.key.functionIndex >= module_.functions.size()) {
            ++statistics_.rejectedTraces;
            return;
        }
        const Function& function = module_.functions[recording.key.functionIndex];
        TraceIRBuilder builder(module_, function, recording.key, recording.events, symbol);
        std::string error;
        const std::optional<std::string> ir = builder.build(error);
        if (!ir.has_value()) {
            statistics_.lastTraceDiagnostic = error;
            ++statistics_.rejectedTraces;
            return;
        }

        auto context = std::make_unique<llvm::LLVMContext>();
        llvm::SMDiagnostic diagnostic;
        const llvm::MemoryBufferRef buffer(*ir, symbol);
        std::unique_ptr<llvm::Module> module = llvm::parseIR(buffer, diagnostic, *context);
        if (!module) {
            std::string message;
            llvm::raw_string_ostream stream(message);
            diagnostic.print("snjit", stream);
            stream.flush();
            statistics_.lastTraceDiagnostic = message;
            ++statistics_.rejectedTraces;
            return;
        }
        module->setTargetTriple(llvmJit_->getTargetTriple());
        module->setDataLayout(llvmJit_->getDataLayout());
        llvm::orc::ThreadSafeModule threadSafeModule(std::move(module), std::move(context));
        if (auto errorValue = llvmJit_->addIRModule(std::move(threadSafeModule))) {
            statistics_.lastTraceDiagnostic = llvm::toString(std::move(errorValue));
            ++statistics_.rejectedTraces;
            return;
        }
        auto address = llvmJit_->lookup(symbol);
        if (!address) {
            statistics_.lastTraceDiagnostic = llvm::toString(address.takeError());
            ++statistics_.rejectedTraces;
            return;
        }
        CompiledTrace trace;
        trace.key = recording.key;
        trace.function = address->toPtr<NativeTraceFunction>();
        trace.slots = builder.slots();
        trace.maxStackDepth = builder.maxStackDepth();
        trace.symbol = symbol;
        traces_.emplace(recording.key, std::move(trace));
        ++statistics_.compiledTraces;
        const auto finished = std::chrono::steady_clock::now();
        statistics_.compilationMicroseconds += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(finished - started).count());
#else
        (void)recording;
        ++statistics_.rejectedTraces;
#endif
    }
};

TracingJIT::TracingJIT(const bytecode::BytecodeModule& module, JitOptions options)
    : impl_(std::make_unique<Impl>(module, options)) {}

TracingJIT::~TracingJIT() = default;
TracingJIT::TracingJIT(TracingJIT&&) noexcept = default;
TracingJIT& TracingJIT::operator=(TracingJIT&&) noexcept = default;

JitResult TracingJIT::run(std::ostream& output) {
    return impl_->run(output);
}

bool TracingJIT::isAvailable() noexcept {
#if SERENITY_HAS_LLVM
    return true;
#else
    return false;
#endif
}

std::string TracingJIT::llvmVersion() {
#if SERENITY_HAS_LLVM
    return LLVM_VERSION_STRING;
#else
    return "unavailable";
#endif
}

std::string formatStatistics(const JitStatistics& statistics) {
    std::ostringstream output;
    output << "Serenity tracing JIT statistics\n"
           << "-------------------------------\n"
           << "Observed loop backedges:       " << statistics.observedBackedges << '\n'
           << "Trace recording attempts:      " << statistics.recordingAttempts << '\n'
           << "Compiled traces:               " << statistics.compiledTraces << '\n'
           << "Rejected traces:               " << statistics.rejectedTraces << '\n'
           << "Trace executions:              " << statistics.traceExecutions << '\n'
           << "Trace iterations:              " << statistics.traceIterations << '\n'
           << "Native bytecode instructions:  " << statistics.nativeBytecodeInstructions << '\n'
           << "Side exits:                    " << statistics.sideExits << '\n'
           << "Instruction-budget exits:      " << statistics.budgetExits << '\n'
           << "Deoptimizations:               " << statistics.deoptimizations << '\n'
           << "Compilation time:              " << statistics.compilationMicroseconds << " us\n";
    if (!statistics.lastTraceDiagnostic.empty()) {
        output << "Last trace diagnostic:          " << statistics.lastTraceDiagnostic << '\n';
    }
    return output.str();
}

} // namespace serenity::jit
