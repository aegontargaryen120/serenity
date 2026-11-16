#include "serenity/aot/llvm_emitter.hpp"
#include "serenity/bytecode/generator.hpp"
#include "serenity/lexer/lexer.hpp"
#include "serenity/parser/parser.hpp"
#include "serenity/preprocessor/preprocessor.hpp"
#include "serenity/semantic/analyzer.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

#ifndef SERENITY_CLANGXX_PATH
#define SERENITY_CLANGXX_PATH "clang++"
#endif

#ifndef SERENITY_RUNTIME_LIBRARY_PATH
#define SERENITY_RUNTIME_LIBRARY_PATH ""
#endif

namespace {

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

[[nodiscard]] std::string shellQuote(const std::string& value) {
    std::string result = "'";
    for (char ch : value) {
        if (ch == '\'') result += "'\\''";
        else result += ch;
    }
    result += '\'';
    return result;
}

struct Options {
    std::filesystem::path input;
    std::filesystem::path output;
    bool emitLLVM {false};
    std::string optimization {"-O0"};
};

void usage(std::ostream& out, const char* executable) {
    out << "Serenity AOT compiler v0.8\n"
        << "Usage: " << executable << " <source.sn> [-o output] [-O0|-O1|-O2|-O3] [--emit-llvm]\n\n"
        << "Options:\n"
        << "  -o <path>       Output executable (or .ll file with --emit-llvm)\n"
        << "  -O0..-O3        Select LLVM optimization level (default: -O0)\n"
        << "  --emit-llvm     Write LLVM IR instead of linking an executable\n"
        << "  -h, --help      Show this help\n"
        << "  --version       Show compiler version\n";
}

bool parseOptions(int argc, char** argv, Options& options, bool& showHelp, bool& showVersion) {
    showHelp = false;
    showVersion = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "-h" || argument == "--help") {
            showHelp = true;
            return true;
        }
        if (argument == "--version") {
            showVersion = true;
            return true;
        }
        if (argument == "--emit-llvm") {
            options.emitLLVM = true;
            continue;
        }
        if (argument == "-o") {
            if (index + 1 >= argc) {
                std::cerr << "error: -o requires an output path\n";
                return false;
            }
            options.output = argv[++index];
            continue;
        }
        if (argument == "-O0" || argument == "-O1" || argument == "-O2" || argument == "-O3") {
            options.optimization = argument;
            continue;
        }
        if (!argument.empty() && argument.front() == '-') {
            std::cerr << "error: unknown option '" << argument << "'\n";
            return false;
        }
        if (!options.input.empty()) {
            std::cerr << "error: only one input source file is supported in v0.8\n";
            return false;
        }
        options.input = argument;
    }
    if (options.input.empty()) {
        std::cerr << "error: no input source file was provided\n";
        return false;
    }
    if (options.output.empty()) {
        options.output = options.input;
        options.output.replace_extension(options.emitLLVM ? ".ll" : "");
        if (options.output.empty()) options.output = options.emitLLVM ? "a.out.ll" : "a.out";
    }
    if (options.emitLLVM && options.output.extension() != ".ll") {
        // Keep the explicitly requested -o path, but the default has already
        // received .ll above. This allows scripts to choose arbitrary names.
    }
    return true;
}

int compile(const Options& options) {
    const std::filesystem::path sourcePath = options.input;
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

    serenity::aot::LLVMIREmitter emitter(*generated.module);
    serenity::aot::LLVMResult lowered = emitter.emit();
    if (!lowered.ok()) {
        for (const serenity::aot::LLVMError& error : lowered.errors) {
            std::cerr << "error: LLVM IR generation: " << error.message << '\n';
        }
        return 1;
    }

    std::error_code filesystemError;
    const std::filesystem::path absoluteOutput = std::filesystem::absolute(options.output, filesystemError);
    if (filesystemError) {
        std::cerr << "error: cannot resolve output path: " << filesystemError.message() << '\n';
        return 1;
    }
    if (!absoluteOutput.parent_path().empty()) {
        std::filesystem::create_directories(absoluteOutput.parent_path(), filesystemError);
        if (filesystemError) {
            std::cerr << "error: cannot create output directory: " << filesystemError.message() << '\n';
            return 1;
        }
    }

    if (options.emitLLVM) {
        std::ofstream output(absoluteOutput, std::ios::binary);
        if (!output) {
            std::cerr << "error: cannot write LLVM IR to '" << absoluteOutput.string() << "'\n";
            return 1;
        }
        output << *lowered.ir;
        if (!output) {
            std::cerr << "error: failed while writing LLVM IR\n";
            return 1;
        }
        std::cout << "Wrote LLVM IR: " << absoluteOutput.string() << '\n';
        return 0;
    }

    const std::filesystem::path runtimeLibrary = SERENITY_RUNTIME_LIBRARY_PATH;
    if (runtimeLibrary.empty() || !std::filesystem::exists(runtimeLibrary)) {
        std::cerr << "error: Serenity runtime library was not found. Rebuild the project with CMake.\n";
        return 1;
    }

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path temporaryIR = std::filesystem::temp_directory_path() /
        ("serenity-aot-" + std::to_string(stamp) + ".ll");
    {
        std::ofstream output(temporaryIR, std::ios::binary);
        if (!output) {
            std::cerr << "error: cannot create temporary LLVM IR file\n";
            return 1;
        }
        output << *lowered.ir;
        if (!output) {
            std::cerr << "error: failed while writing temporary LLVM IR\n";
            std::filesystem::remove(temporaryIR, filesystemError);
            return 1;
        }
    }

    const std::string command = shellQuote(SERENITY_CLANGXX_PATH) + " " + options.optimization +
        " -Wno-override-module " + shellQuote(temporaryIR.string()) + " " + shellQuote(runtimeLibrary.string()) +
        " -o " + shellQuote(absoluteOutput.string());
    const int compilerStatus = std::system(command.c_str());
    std::filesystem::remove(temporaryIR, filesystemError);
    if (compilerStatus != 0) {
        std::cerr << "error: LLVM native code generation/linking failed (command status "
                  << compilerStatus << ")\n";
        return 1;
    }

    std::cout << "Generated native executable: " << absoluteOutput.string() << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    bool showHelp = false;
    bool showVersion = false;
    if (!parseOptions(argc, argv, options, showHelp, showVersion)) {
        usage(std::cerr, argv[0]);
        return 2;
    }
    if (showHelp) {
        usage(std::cout, argv[0]);
        return 0;
    }
    if (showVersion) {
        std::cout << "Serenity AOT compiler 0.8.0\n";
        return 0;
    }
    return compile(options);
}
