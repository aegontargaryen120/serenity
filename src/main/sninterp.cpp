#include "serenity/bytecode/generator.hpp"
#include "serenity/interpreter/interpreter.hpp"
#include "serenity/lexer/lexer.hpp"
#include "serenity/parser/parser.hpp"
#include "serenity/preprocessor/preprocessor.hpp"
#include "serenity/semantic/analyzer.hpp"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <variant>

namespace {

void printLocation(
    const std::filesystem::path& file, const serenity::SourceLocation& location,
    const std::string& message) {
    std::cerr << file.string() << ':' << location.line << ':' << location.column
              << ": error: " << message << '\n';
}

void printExpandedLocation(
    const serenity::PreprocessResult& preprocessed,
    const std::filesystem::path& fallback,
    const serenity::SourceLocation& location,
    const std::string& message) {
    if (const auto origin = preprocessed.originAt(location.offset); origin.has_value()) {
        printLocation(origin->file, origin->location, message);
    } else {
        printLocation(fallback, location, message);
    }
}

int reportPreprocessDiagnostics(const serenity::PreprocessResult& result) {
    bool hasErrors = false;
    for (const serenity::PreprocessDiagnostic& diagnostic : result.diagnostics) {
        if (diagnostic.severity == serenity::PreprocessSeverity::Error) hasErrors = true;
        const char* severity = diagnostic.severity == serenity::PreprocessSeverity::Error
            ? "error" : "warning";
        std::cerr << diagnostic.origin.file.string() << ':'
                  << diagnostic.origin.location.line << ':'
                  << diagnostic.origin.location.column << ": " << severity << ": "
                  << diagnostic.message << '\n';
    }
    return hasErrors ? 1 : 0;
}

int usage(const char* executable) {
    std::cerr << "Usage: " << executable << " <source.sn> [--stats]\n";
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
        std::cout << "Serenity bytecode interpreter\n"
                  << "Usage: sninterp <source.sn> [--stats]\n";
        return 0;
    }
    if (argc < 2 || argc > 3) return usage(argv[0]);
    const std::filesystem::path sourcePath = argv[1];
    const bool showStatistics = argc == 3 && std::string(argv[2]) == "--stats";
    if (argc == 3 && !showStatistics) return usage(argv[0]);
    serenity::Preprocessor preprocessor;
    serenity::PreprocessResult preprocessed = preprocessor.preprocessFile(sourcePath);
    if (reportPreprocessDiagnostics(preprocessed) != 0 || !preprocessed.ok()) return 1;

    serenity::Lexer lexer(preprocessed.source);
    serenity::LexResult lexed = lexer.tokenize();
    if (!lexed.ok()) {
        for (const serenity::LexError& error : lexed.errors) {
            printExpandedLocation(preprocessed, sourcePath, error.location, error.message);
        }
        return 1;
    }

    serenity::parser::Parser parser(std::move(lexed.tokens));
    serenity::parser::ParseResult parsed = parser.parse();
    if (!parsed.ok()) {
        for (const serenity::parser::ParseError& error : parsed.errors) {
            printExpandedLocation(preprocessed, sourcePath, error.location, error.message);
        }
        return 1;
    }

    serenity::semantic::SemanticAnalyzer analyzer(*parsed.program);
    serenity::semantic::SemanticResult checked = analyzer.analyze();
    if (!checked.ok()) {
        for (const serenity::semantic::SemanticError& error : checked.errors) {
            printExpandedLocation(preprocessed, sourcePath, error.location, error.message);
        }
        return 1;
    }

    serenity::bytecode::BytecodeGenerator generator(*parsed.program, checked.model);
    serenity::bytecode::BytecodeResult generated = generator.generate();
    if (!generated.ok()) {
        for (const serenity::bytecode::BytecodeError& error : generated.errors) {
            printExpandedLocation(preprocessed, sourcePath, error.location, error.message);
        }
        return 1;
    }

    serenity::interpreter::Interpreter interpreter(*generated.module, std::cout);
    serenity::interpreter::ExecutionResult execution = interpreter.run();
    if (!execution.ok) {
        if (execution.error.has_value()) {
            const serenity::interpreter::RuntimeError& error = *execution.error;
            std::cerr << "runtime error in " << error.functionName
                      << " at instruction " << error.instructionIndex;
            if (error.span.begin.line != 0) {
                if (const auto origin = preprocessed.originAt(error.span.begin.offset); origin.has_value()) {
                    std::cerr << " (" << origin->file.string() << ':'
                              << origin->location.line << ':' << origin->location.column << ')';
                } else {
                    std::cerr << " (" << sourcePath.string() << ':'
                              << error.span.begin.line << ':' << error.span.begin.column << ')';
                }
            }
            std::cerr << ": " << error.message << '\n';
        } else {
            std::cerr << "runtime error: execution failed\n";
        }
        if (showStatistics) {
            std::cerr << "Serenity interpreter statistics\n"
                      << "Executed bytecode instructions: " << execution.stats.executedInstructions << '\n'
                      << "Function calls:                 " << execution.stats.functionCalls << '\n'
                      << "Peak operand stack:             " << execution.stats.peakOperandStack << '\n'
                      << "Peak call depth:                " << execution.stats.peakCallDepth << '\n';
        }
        return 1;
    }

    if (showStatistics) {
        std::cerr << "Serenity interpreter statistics\n"
                  << "Executed bytecode instructions: " << execution.stats.executedInstructions << '\n'
                  << "Function calls:                 " << execution.stats.functionCalls << '\n'
                  << "Peak operand stack:             " << execution.stats.peakOperandStack << '\n'
                  << "Peak call depth:                " << execution.stats.peakCallDepth << '\n';
    }

    if (execution.returnValue.has_value()) {
        if (const auto* exitCode = std::get_if<std::int64_t>(&execution.returnValue->data)) {
            // Unix process exit codes are conventionally restricted to one byte.
            return static_cast<int>(static_cast<std::uint8_t>(*exitCode));
        }
    }
    return 0;
}
