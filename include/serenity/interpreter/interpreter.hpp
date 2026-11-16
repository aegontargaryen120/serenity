#pragma once

#include "serenity/bytecode/module.hpp"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace serenity::interpreter {

struct StructInstance;

struct EnumValue {
    std::uint32_t enumIndex {0};
    std::uint32_t memberIndex {0};

    friend bool operator==(const EnumValue&, const EnumValue&) = default;
};

// Runtime values are deliberately independent of LLVM. Struct values use
// shared ownership so copying a struct reference preserves identity and field
// mutations are visible through every alias.
struct Value {
    using Storage = std::variant<
        std::monostate,
        std::int64_t,
        float,
        double,
        bool,
        std::string,
        EnumValue,
        std::shared_ptr<StructInstance>>;

    Storage data;

    Value() = default;
    explicit Value(Storage value) : data(std::move(value)) {}
};

struct StructInstance {
    std::uint32_t descriptorIndex {0};
    std::vector<Value> fields;
};

struct RuntimeError {
    std::string message;
    std::uint32_t functionIndex {0};
    std::string functionName;
    std::size_t instructionIndex {0};
    ast::SourceSpan span {};
};

struct RuntimeStats {
    std::uint64_t executedInstructions {0};
    std::uint64_t functionCalls {0};
    std::size_t peakOperandStack {0};
    std::size_t peakCallDepth {0};
};

struct ExecutionResult {
    bool ok {false};
    std::optional<Value> returnValue;
    std::optional<RuntimeError> error;
    RuntimeStats stats;
};

struct InterpreterOptions {
    // A safety ceiling prevents an accidental infinite loop from hanging the
    // command-line interpreter indefinitely. Set to zero to disable it.
    std::uint64_t maxInstructions {100'000'000};
    std::size_t maxCallDepth {1024};
};

class Interpreter {
public:
    explicit Interpreter(
        const bytecode::BytecodeModule& module,
        std::ostream& output,
        InterpreterOptions options = {});

    // Runs the synthetic global initializer (when present), then the module's
    // recorded entry function. A module without an entry function is rejected.
    [[nodiscard]] ExecutionResult run();

private:
    struct Frame {
        std::uint32_t functionIndex {0};
        std::size_t instructionPointer {0};
        std::size_t stackBase {0};
        std::vector<Value> locals;
    };

    const bytecode::BytecodeModule& module_;
    std::ostream& output_;
    InterpreterOptions options_;

    std::vector<Value> operandStack_;
    std::vector<Value> globals_;
    std::vector<Frame> frames_;
    RuntimeStats stats_;
    std::optional<RuntimeError> error_;
    std::optional<Value> rootReturnValue_;

    void reset();
    [[nodiscard]] bool executeFunction(std::uint32_t functionIndex, std::vector<Value> arguments,
                                       std::size_t stackBase);
    [[nodiscard]] bool executeLoop(std::size_t stopDepth);
    [[nodiscard]] bool pushFrame(std::uint32_t functionIndex, std::vector<Value> arguments,
                                 std::size_t stackBase);
    void fail(std::string message, std::uint32_t functionIndex, std::size_t instructionIndex);

    [[nodiscard]] bool pop(Value& value, std::uint32_t functionIndex, std::size_t instructionIndex);
    [[nodiscard]] bool popTwo(Value& left, Value& right, std::uint32_t functionIndex,
                              std::size_t instructionIndex);
    void push(Value value);

    [[nodiscard]] Value defaultValue(const bytecode::TypeInfo& type) const;
    [[nodiscard]] Value constantValue(const bytecode::Constant& constant) const;
    [[nodiscard]] bool valueMatchesType(const Value& value, const bytecode::TypeInfo& type) const;
    [[nodiscard]] bool executeInstruction(const bytecode::Instruction& instruction,
                                          std::uint32_t functionIndex,
                                          std::size_t instructionIndex);
    [[nodiscard]] bool executeArithmetic(const bytecode::Instruction& instruction,
                                         std::uint32_t functionIndex,
                                         std::size_t instructionIndex);
    [[nodiscard]] bool executeComparison(const bytecode::Instruction& instruction,
                                         std::uint32_t functionIndex,
                                         std::size_t instructionIndex);
    [[nodiscard]] bool executeBitwise(const bytecode::Instruction& instruction,
                                      std::uint32_t functionIndex,
                                      std::size_t instructionIndex);
    [[nodiscard]] bool executeReturn(const bytecode::Instruction& instruction,
                                     std::uint32_t functionIndex,
                                     std::size_t instructionIndex);
    [[nodiscard]] bool executeCall(const bytecode::Instruction& instruction,
                                   std::uint32_t functionIndex,
                                   std::size_t instructionIndex);
    [[nodiscard]] bool executeNewStruct(const bytecode::Instruction& instruction,
                                        std::uint32_t functionIndex,
                                        std::size_t instructionIndex);
    [[nodiscard]] bool executeField(const bytecode::Instruction& instruction,
                                    std::uint32_t functionIndex,
                                    std::size_t instructionIndex);

    [[nodiscard]] static bool isComparison(bytecode::Opcode opcode) noexcept;
    [[nodiscard]] static bool isBitwise(bytecode::Opcode opcode) noexcept;
};

} // namespace serenity::interpreter
