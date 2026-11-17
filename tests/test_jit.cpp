#include "serenity/bytecode/generator.hpp"
#include "serenity/interpreter/interpreter.hpp"
#include "serenity/jit/jit.hpp"
#include "serenity/lexer/lexer.hpp"
#include "serenity/parser/parser.hpp"
#include "serenity/semantic/analyzer.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

serenity::bytecode::BytecodeModule compile(const std::string& source) {
    serenity::Lexer lexer(source);
    auto lexed = lexer.tokenize();
    if (!lexed.ok()) {
        for (const auto& error : lexed.errors) std::cerr << "lex: " << error.message << '\n';
        std::exit(EXIT_FAILURE);
    }
    serenity::parser::Parser parser(std::move(lexed.tokens));
    auto parsed = parser.parse();
    if (!parsed.ok()) {
        for (const auto& error : parsed.errors) std::cerr << "parse: " << error.message << '\n';
        std::exit(EXIT_FAILURE);
    }
    serenity::semantic::SemanticAnalyzer analyzer(*parsed.program);
    auto checked = analyzer.analyze();
    if (!checked.ok()) {
        for (const auto& error : checked.errors) std::cerr << "semantic: " << error.message << '\n';
        std::exit(EXIT_FAILURE);
    }
    serenity::bytecode::BytecodeGenerator generator(*parsed.program, checked.model);
    auto generated = generator.generate();
    if (!generated.ok()) {
        for (const auto& error : generated.errors) std::cerr << "bytecode: " << error.message << '\n';
        std::exit(EXIT_FAILURE);
    }
    return std::move(*generated.module);
}

class RecursiveFrameProbe final : public serenity::interpreter::InterpreterHooks {
public:
    bool tryExecuteTrace(
        std::uint32_t, std::size_t, const serenity::bytecode::Function&,
        std::vector<serenity::interpreter::Value>&,
        std::vector<serenity::interpreter::Value>&,
        std::vector<serenity::interpreter::Value>&, std::size_t, std::size_t&,
        std::uint64_t, std::uint64_t&) override {
        return false;
    }

    void onInstructionExecuted(
        std::uint32_t functionIndex, std::size_t,
        const serenity::bytecode::Instruction& instruction,
        std::optional<bool>, std::size_t nextInstructionPointer,
        std::size_t relativeStackDepth) override {
        if (functionIndex == recursiveFunctionIndex &&
            instruction.opcode == serenity::bytecode::Opcode::Call &&
            nextInstructionPointer == invalidIndex && relativeStackDepth == invalidIndex) {
            sawRecursiveFrameChange = true;
        }
    }

    std::uint32_t recursiveFunctionIndex {0};
    bool sawRecursiveFrameChange {false};
    static constexpr std::size_t invalidIndex = static_cast<std::size_t>(-1);
};

void verifyRecursiveFrameNotifications() {
    const auto module = compile(R"SERENITY(
func descend(int depth) => int {
    if (depth > 0) {
        return descend(depth - 1);
    }
    return 0;
}

func main() => int {
    return descend(3);
}
)SERENITY");

    RecursiveFrameProbe probe;
    for (std::size_t index = 0; index < module.functions.size(); ++index) {
        if (module.functions[index].name == "descend") {
            probe.recursiveFunctionIndex = static_cast<std::uint32_t>(index);
            break;
        }
    }

    serenity::interpreter::InterpreterOptions options;
    options.hooks = &probe;
    std::ostringstream output;
    serenity::interpreter::Interpreter interpreter(module, output, options);
    const auto execution = interpreter.run();
    require(execution.ok, "recursive function should execute with observer hooks");
    require(probe.sawRecursiveFrameChange,
            "recursive call to the same function must be reported as a frame change");
}

} // namespace

int main() {
    verifyRecursiveFrameNotifications();

    const std::string loopSource = R"SERENITY(
func main() => int {
    mut int total = 0;
    for (mut int cursor = 0; cursor < 1000; cursor += 1) {
        total += cursor;
    }
    println(total);
    return 0;
}
)SERENITY";
    const auto module = compile(loopSource);

    std::ostringstream interpreterOutput;
    serenity::interpreter::Interpreter interpreter(module, interpreterOutput);
    const auto interpreted = interpreter.run();
    require(interpreted.ok, "interpreter baseline must execute the test program");
    require(interpreterOutput.str() == "499500\n", "interpreter baseline output must be correct");

    serenity::jit::JitOptions options;
    options.hotLoopThreshold = 2;
    options.maxInstructions = 5'000'000;
    std::ostringstream jitOutput;
    serenity::jit::TracingJIT jit(module, options);
    const auto compiled = jit.run(jitOutput);

    if (!serenity::jit::TracingJIT::isAvailable()) {
        require(!compiled.jitAvailable, "build without LLVM should report JIT unavailable explicitly");
        std::cout << "LLVM not found: Serenity ORC execution tests skipped.\n";
        return EXIT_SUCCESS;
    }

    require(compiled.jitAvailable, "LLVM ORC JIT should initialize");
    require(compiled.execution.ok, "JIT execution must complete successfully");
    require(jitOutput.str() == interpreterOutput.str(),
            "JIT and interpreter must produce identical output");
    require(compiled.jit.compiledTraces >= 1,
            "the C-style for loop should produce at least one compiled trace");
    require(compiled.jit.traceExecutions >= 1,
            "compiled loop trace should execute");
    require(compiled.jit.nativeBytecodeInstructions > 100,
            "compiled trace should account for native bytecode instructions");
    require(compiled.jit.traceIterations >= 100,
            "compiled trace should run multiple loop iterations natively");
    require(compiled.jit.sideExits >= 1,
            "loop termination should side-exit back to the interpreter");

    // An arithmetic deoptimization must restore the complete operand stack,
    // including values below the operands consumed by the failing operation.
    const auto deoptModule = compile(R"SERENITY(
func main() => int {
    mut int value = 9223372036854775697;
    mut int cursor = 0;
    while (cursor < 12) {
        value = 10 + (value + 1);
        cursor += 1;
    }
    return 0;
}
)SERENITY");

    std::ostringstream deoptInterpreterOutput;
    serenity::interpreter::Interpreter deoptInterpreter(deoptModule, deoptInterpreterOutput);
    const auto deoptInterpreted = deoptInterpreter.run();
    require(!deoptInterpreted.ok && deoptInterpreted.error.has_value(),
            "interpreter baseline should detect the intentional integer overflow");

    serenity::jit::JitOptions deoptOptions;
    deoptOptions.hotLoopThreshold = 1;
    deoptOptions.maxInstructions = 1'000'000;
    std::ostringstream deoptJitOutput;
    serenity::jit::TracingJIT deoptJit(deoptModule, deoptOptions);
    const auto deoptimized = deoptJit.run(deoptJitOutput);
    require(deoptimized.jitAvailable, "deoptimization test requires LLVM ORC");
    require(!deoptimized.execution.ok && deoptimized.execution.error.has_value(),
            "JIT must preserve the interpreter's integer-overflow error");
    require(deoptimized.execution.error->message == deoptInterpreted.error->message,
            "JIT deoptimization must preserve the original runtime diagnostic");
    require(deoptimized.jit.compiledTraces >= 1,
            "overflow test should compile its hot loop before deoptimizing");
    require(deoptimized.jit.deoptimizations >= 1,
            "overflow guard should deoptimize to the interpreter");

    std::cout << "All Serenity tracing JIT tests passed.\n";
    return EXIT_SUCCESS;
}
