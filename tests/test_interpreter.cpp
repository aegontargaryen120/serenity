#include "serenity/bytecode/generator.hpp"
#include "serenity/interpreter/interpreter.hpp"
#include "serenity/lexer/lexer.hpp"
#include "serenity/parser/parser.hpp"
#include "serenity/semantic/analyzer.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <variant>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

struct CompiledProgram {
    serenity::bytecode::BytecodeModule module;
};

CompiledProgram compile(const std::string& source) {
    serenity::Lexer lexer(source);
    auto lexed = lexer.tokenize();
    if (!lexed.ok()) {
        for (const auto& error : lexed.errors) {
            std::cerr << error.location.line << ':' << error.location.column
                      << ": lexical: " << error.message << '\n';
        }
        std::exit(EXIT_FAILURE);
    }

    serenity::parser::Parser parser(std::move(lexed.tokens));
    auto parsed = parser.parse();
    if (!parsed.ok()) {
        for (const auto& error : parsed.errors) {
            std::cerr << error.location.line << ':' << error.location.column
                      << ": parse: " << error.message << '\n';
        }
        std::exit(EXIT_FAILURE);
    }

    serenity::semantic::SemanticAnalyzer analyzer(*parsed.program);
    auto checked = analyzer.analyze();
    if (!checked.ok()) {
        for (const auto& error : checked.errors) {
            std::cerr << error.location.line << ':' << error.location.column
                      << ": semantic: " << error.message << '\n';
        }
        std::exit(EXIT_FAILURE);
    }

    serenity::bytecode::BytecodeGenerator generator(*parsed.program, checked.model);
    auto generated = generator.generate();
    if (!generated.ok()) {
        for (const auto& error : generated.errors) {
            std::cerr << error.location.line << ':' << error.location.column
                      << ": bytecode: " << error.message << '\n';
        }
        std::exit(EXIT_FAILURE);
    }
    return {std::move(*generated.module)};
}

} // namespace

int main() {
    using serenity::interpreter::Interpreter;
    using serenity::interpreter::InterpreterOptions;

    const std::string source = R"SERENITY(
mut int seed = 2;
int untouchedGlobal;

struct {
    int count = 4;
    string label = "initial";
} Counter;

enum { North, South } Direction;

func add(int left, int right) => int {
    return left + right;
}

func factorial(int n) => int {
    if (n <= 1) {
        return 1;
    }
    return n * factorial(n - 1);
}

func bump() => bool {
    seed += 100;
    return true;
}

func main() => int {
    mut int total = add(seed, 3);
    total += factorial(5);

    mut Counter counter = {
        .count = total,
        .label = "Serenity"
    };
    counter.count += 1;

    Direction direction = Direction.South;
    bool no = false && bump();
    bool yes = true || bump();
    bool nullableComparison = (counter.label =/= null) && (direction == Direction.South);
    bool defaultFlag;
    string emptyString;
    bool defaultsOkay = (defaultFlag == false) && (emptyString == null) && (untouchedGlobal == 0);
    Counter nullCounter;
    Direction nullDirection;
    bool nullableDefaultsOkay = (nullCounter == null) && (nullDirection == null);
    Counter defaults = {};
    bool fieldDefaultsOkay = (defaults.count == 4) && (defaults.label == "initial");
    int bitwise = (8 >> 1) | (1 << 2);
    mut int remainder = 10;
    remainder %%= 4;

    mut int cursor = 0;
    mut int accumulated = 0;
    while (cursor < 5) {
        cursor += 1;
        if (cursor == 2) {
            continue;
        }
        if (cursor == 4) {
            break;
        }
        accumulated += cursor;
    }

    float single = 1.5;
    float wide = 2.0;
    double precise = 2.0;
    double squared = precise * precise;

    if (single < wide && no == false && yes && nullableComparison && defaultsOkay &&
        nullableDefaultsOkay && fieldDefaultsOkay && bitwise == 4 && remainder == 2 &&
        accumulated == 4) {
        println(counter.label);
        println(counter.count);
        println("INTERPRETER OK");
    } else {
        println("INTERPRETER FAILED");
    }

    while (total < 126) {
        total += 1;
    }

    return 0;
}
)SERENITY";

    CompiledProgram compiled = compile(source);
    std::ostringstream output;
    Interpreter interpreter(compiled.module, output);
    const auto execution = interpreter.run();

    require(execution.ok, "valid bytecode program should execute successfully");
    require(output.str() == "Serenity\n126\nINTERPRETER OK\n",
            "interpreter should execute arithmetic, recursion, structs, enums, branches and loops");
    require(execution.returnValue.has_value(), "main's return value should be retained");
    const auto* exitCode = std::get_if<std::int64_t>(&execution.returnValue->data);
    require(exitCode != nullptr && *exitCode == 0, "main should return integer zero");
    require(execution.stats.executedInstructions > 50, "instruction counter should be populated");
    require(execution.stats.functionCalls >= 7, "function-call statistics should include recursion");
    require(execution.stats.peakCallDepth >= 6, "call-depth statistics should include recursive frames");
    require(execution.stats.peakOperandStack > 0, "operand-stack high-water mark should be populated");

    const std::string divisionByZero = R"SERENITY(
func main() => int {
    return 10 / 0;
}
)SERENITY";
    CompiledProgram badArithmetic = compile(divisionByZero);
    std::ostringstream ignoredOutput;
    Interpreter badInterpreter(badArithmetic.module, ignoredOutput);
    const auto badExecution = badInterpreter.run();
    require(!badExecution.ok, "division by zero should produce a runtime error");
    require(badExecution.error.has_value(), "runtime failure should include diagnostics");
    require(badExecution.error->message.find("division by zero") != std::string::npos,
            "division-by-zero diagnostic should identify the operation");
    require(badExecution.error->span.begin.line != 0,
            "runtime diagnostics should retain bytecode source locations");

    const std::string endlessLoop = R"SERENITY(
func main() => int {
    while (true) {
    }
    return 0;
}
)SERENITY";
    CompiledProgram looping = compile(endlessLoop);
    InterpreterOptions limited;
    limited.maxInstructions = 20;
    std::ostringstream loopOutput;
    Interpreter limitedInterpreter(looping.module, loopOutput, limited);
    const auto loopExecution = limitedInterpreter.run();
    require(!loopExecution.ok, "instruction limit should stop an infinite loop");
    require(loopExecution.error.has_value(), "instruction-limit failure should include diagnostics");
    require(loopExecution.error->message.find("maximum instruction count") != std::string::npos,
            "instruction-limit diagnostic should explain why execution stopped");

    std::cout << "All Serenity interpreter tests passed.\n";
    return EXIT_SUCCESS;
}
