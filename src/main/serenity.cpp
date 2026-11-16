#include "serenity/aot/llvm_backend.hpp"
#include "serenity/aot/llvm_emitter.hpp"
#include "serenity/bytecode/generator.hpp"
#include "serenity/lexer/lexer.hpp"
#include "serenity/parser/parser.hpp"
#include "serenity/preprocessor/preprocessor.hpp"
#include "serenity/semantic/analyzer.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

#ifndef SERENITY_RUNTIME_LIBRARY_PATH
#define SERENITY_RUNTIME_LIBRARY_PATH ""
#endif
#ifndef SERENITY_LINKER_PATH
#define SERENITY_LINKER_PATH ""
#endif
#ifndef SERENITY_MACOS_SDKROOT
#define SERENITY_MACOS_SDKROOT ""
#endif
#ifndef SERENITY_MACOS_SDK_VERSION
#define SERENITY_MACOS_SDK_VERSION ""
#endif
#ifndef SERENITY_MACOS_MIN_VERSION
#define SERENITY_MACOS_MIN_VERSION "13.0"
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

struct Options {
    std::filesystem::path input;
    std::filesystem::path output;
    bool emitLLVM {false};
    bool emitObject {false};
    std::string optimization {"-O0"};
};

void usage(std::ostream& out, const char* executable) {
    out << "Serenity AOT compiler v0.11.0\n"
        << "Usage: " << executable << " <source.sn> [-o output] [-O0|-O1|-O2|-O3] [--emit-llvm|--emit-object]\n\n"
        << "Options:\n"
        << "  -o <path>       Output executable, .ll file, or object file\n"
        << "  -O0..-O3        Select LLVM optimization level (default: -O0)\n"
        << "  --emit-llvm     Write textual LLVM IR without compiling it\n"
        << "  --emit-object   Write a native object file without linking\n"
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
            if (options.emitObject) {
                std::cerr << "error: --emit-llvm and --emit-object are mutually exclusive\n";
                return false;
            }
            options.emitLLVM = true;
            continue;
        }
        if (argument == "--emit-object") {
            if (options.emitLLVM) {
                std::cerr << "error: --emit-llvm and --emit-object are mutually exclusive\n";
                return false;
            }
            options.emitObject = true;
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
            std::cerr << "error: only one input source file is supported\n";
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
        if (options.emitLLVM) options.output.replace_extension(".ll");
        else if (options.emitObject) options.output.replace_extension(".o");
        else options.output.replace_extension("");
        if (options.output.empty()) {
            options.output = options.emitLLVM ? "a.out.ll" : (options.emitObject ? "a.out.o" : "a.out");
        }
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

    std::filesystem::path objectPath = absoluteOutput;
    if (!options.emitObject) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        objectPath = std::filesystem::temp_directory_path() /
            ("serenity-aot-" + std::to_string(stamp) + ".o");
    }

    const serenity::aot::LLVMBackendResult objectResult =
        serenity::aot::LLVMBackend::emitObjectFile(*lowered.ir, objectPath, options.optimization);
    if (!objectResult.ok()) {
        std::cerr << "error: LLVM native object generation: " << objectResult.message << '\n';
        return 1;
    }

    if (options.emitObject) {
        std::cout << "Generated native object file: " << absoluteOutput.string() << '\n';
        return 0;
    }

    const std::filesystem::path runtimeLibrary = SERENITY_RUNTIME_LIBRARY_PATH;
    if (runtimeLibrary.empty() || !std::filesystem::exists(runtimeLibrary)) {
        std::filesystem::remove(objectPath, filesystemError);
        std::cerr << "error: Serenity runtime library was not found. Rebuild the project with CMake.\n";
        return 1;
    }

    serenity::aot::NativeLinkOptions linkOptions;
    linkOptions.linker = SERENITY_LINKER_PATH;
    linkOptions.sdkRoot = SERENITY_MACOS_SDKROOT;
    linkOptions.sdkVersion = SERENITY_MACOS_SDK_VERSION;
    linkOptions.minimumMacOSVersion = SERENITY_MACOS_MIN_VERSION;
    const serenity::aot::LLVMBackendResult linkResult =
        serenity::aot::LLVMBackend::linkExecutable(objectPath, runtimeLibrary, absoluteOutput, linkOptions);
    std::filesystem::remove(objectPath, filesystemError);
    if (!linkResult.ok()) {
        std::cerr << "error: native executable linking: " << linkResult.message << '\n';
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
        std::cout << "Serenity AOT compiler 0.11.0\n";
        return 0;
    }
    return compile(options);
}
