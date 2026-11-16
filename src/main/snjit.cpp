#include "serenity/bytecode/generator.hpp"
#include "serenity/jit/jit.hpp"
#include "serenity/lexer/lexer.hpp"
#include "serenity/parser/parser.hpp"
#include "serenity/preprocessor/preprocessor.hpp"
#include "serenity/semantic/analyzer.hpp"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

struct Arguments {
    std::filesystem::path source;
    serenity::jit::JitOptions jit;
};

[[nodiscard]] bool parseUnsigned(std::string_view text, std::uint64_t& value) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

[[nodiscard]] bool parseArguments(int argc, char** argv, Arguments& arguments) {
    if (argc < 2) return false;
    arguments.source = argv[1];
    for (int index = 2; index < argc; ++index) {
        const std::string_view option = argv[index];
        if (option == "--stats") {
            arguments.jit.showStatistics = true;
        } else if (option == "--hot-threshold" && index + 1 < argc) {
            std::uint64_t value = 0;
            if (!parseUnsigned(argv[++index], value) || value == 0) return false;
            arguments.jit.hotLoopThreshold = value;
        } else if (option == "--max-instructions" && index + 1 < argc) {
            std::uint64_t value = 0;
            if (!parseUnsigned(argv[++index], value)) return false;
            arguments.jit.maxInstructions = value;
        } else {
            return false;
        }
    }
    return true;
}

void printLocation(const std::filesystem::path& file,
                   const serenity::SourceLocation& location,
                   const std::string& message) {
    std::cerr << file.string() << ':' << location.line << ':' << location.column
              << ": error: " << message << '\n';
}

void printExpandedLocation(const serenity::PreprocessResult& preprocessed,
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

void printUsage(std::ostream& output, const char* executable) {
    output << "Usage: " << executable << " <source.sn> [options]\n"
           << "Options:\n"
           << "  --hot-threshold N       Backedges before recording a loop (default: 8)\n"
           << "  --max-instructions N    Execution safety limit (default: 100000000; 0 disables)\n"
           << "  --stats                 Print interpreter/JIT statistics to stderr\n"
           << "  --help                  Display this help\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && (std::string_view(argv[1]) == "--help" ||
                      std::string_view(argv[1]) == "-h")) {
        std::cout << "Serenity tracing JIT (LLVM " << serenity::jit::TracingJIT::llvmVersion() << ")\n";
        printUsage(std::cout, argv[0]);
        return 0;
    }

    Arguments arguments;
    if (!parseArguments(argc, argv, arguments)) {
        printUsage(std::cerr, argv[0]);
        return 2;
    }

    serenity::Preprocessor preprocessor;
    serenity::PreprocessResult preprocessed = preprocessor.preprocessFile(arguments.source);
    if (reportPreprocessDiagnostics(preprocessed) != 0 || !preprocessed.ok()) return 1;

    serenity::Lexer lexer(preprocessed.source);
    serenity::LexResult lexed = lexer.tokenize();
    if (!lexed.ok()) {
        for (const serenity::LexError& error : lexed.errors) {
            printExpandedLocation(preprocessed, arguments.source, error.location, error.message);
        }
        return 1;
    }

    serenity::parser::Parser parser(std::move(lexed.tokens));
    serenity::parser::ParseResult parsed = parser.parse();
    if (!parsed.ok()) {
        for (const serenity::parser::ParseError& error : parsed.errors) {
            printExpandedLocation(preprocessed, arguments.source, error.location, error.message);
        }
        return 1;
    }

    serenity::semantic::SemanticAnalyzer analyzer(*parsed.program);
    serenity::semantic::SemanticResult checked = analyzer.analyze();
    if (!checked.ok()) {
        for (const serenity::semantic::SemanticError& error : checked.errors) {
            printExpandedLocation(preprocessed, arguments.source, error.location, error.message);
        }
        return 1;
    }

    serenity::bytecode::BytecodeGenerator generator(*parsed.program, checked.model);
    serenity::bytecode::BytecodeResult generated = generator.generate();
    if (!generated.ok()) {
        for (const serenity::bytecode::BytecodeError& error : generated.errors) {
            printExpandedLocation(preprocessed, arguments.source, error.location, error.message);
        }
        return 1;
    }

    serenity::jit::TracingJIT jit(*generated.module, arguments.jit);
    serenity::jit::JitResult result = jit.run(std::cout);
    if (!result.jitAvailable) {
        std::cerr << "snjit: error: " << result.initializationError << '\n';
        return 1;
    }

    if (!result.execution.ok) {
        if (result.execution.error.has_value()) {
            const serenity::interpreter::RuntimeError& error = *result.execution.error;
            std::cerr << "runtime error in " << error.functionName
                      << " at instruction " << error.instructionIndex;
            if (error.span.begin.line != 0) {
                if (const auto origin = preprocessed.originAt(error.span.begin.offset); origin.has_value()) {
                    std::cerr << " (" << origin->file.string() << ':'
                              << origin->location.line << ':' << origin->location.column << ')';
                } else {
                    std::cerr << " (" << arguments.source.string() << ':'
                              << error.span.begin.line << ':' << error.span.begin.column << ')';
                }
            }
            std::cerr << ": " << error.message << '\n';
        } else {
            std::cerr << "runtime error: execution failed\n";
        }
        if (arguments.jit.showStatistics) {
            std::cerr << "Total executed bytecode instructions: "
                      << result.execution.stats.executedInstructions << '\n'
                      << "Interpreted bytecode instructions:    "
                      << (result.execution.stats.executedInstructions >= result.jit.nativeBytecodeInstructions
                          ? result.execution.stats.executedInstructions - result.jit.nativeBytecodeInstructions : 0)
                      << '\n'
                      << serenity::jit::formatStatistics(result.jit);
        }
        return 1;
    }

    if (arguments.jit.showStatistics) {
        std::cerr << "Total executed bytecode instructions: "
                  << result.execution.stats.executedInstructions << '\n'
                  << "Interpreted bytecode instructions:    "
                  << (result.execution.stats.executedInstructions >= result.jit.nativeBytecodeInstructions
                      ? result.execution.stats.executedInstructions - result.jit.nativeBytecodeInstructions : 0)
                  << '\n'
                  << serenity::jit::formatStatistics(result.jit);
    }

    if (result.execution.returnValue.has_value()) {
        if (const auto* exitCode = std::get_if<std::int64_t>(&result.execution.returnValue->data)) {
            return static_cast<int>(static_cast<std::uint8_t>(*exitCode));
        }
    }
    return 0;
}
