#include "serenity/bytecode/generator.hpp"
#include "serenity/interpreter/interpreter.hpp"
#include "serenity/lexer/lexer.hpp"
#include "serenity/parser/parser.hpp"
#include "serenity/preprocessor/preprocessor.hpp"
#include "serenity/semantic/analyzer.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>

#ifndef SERENITY_STDLIB_TEST_SOURCE
#define SERENITY_STDLIB_TEST_SOURCE ""
#endif

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    const std::filesystem::path sourcePath {SERENITY_STDLIB_TEST_SOURCE};
    require(!sourcePath.empty(), "standard-library test source path must be configured");

    serenity::Preprocessor preprocessor;
    const serenity::PreprocessResult preprocessed = preprocessor.preprocessFile(sourcePath);
    if (!preprocessed.ok()) {
        for (const auto& diagnostic : preprocessed.diagnostics) {
            std::cerr << diagnostic.origin.file << ':' << diagnostic.origin.location.line
                      << ':' << diagnostic.origin.location.column << ": "
                      << diagnostic.message << '\n';
        }
    }
    require(preprocessed.ok(), "standard-library header should resolve and preprocess");
    require(preprocessed.source.find("func absInt") != std::string::npos,
            "math header should be textually expanded");

    serenity::Lexer lexer(preprocessed.source);
    auto lexed = lexer.tokenize();
    require(lexed.ok(), "standard-library example should tokenize");

    serenity::parser::Parser parser(std::move(lexed.tokens));
    auto parsed = parser.parse();
    require(parsed.ok(), "standard-library example should parse");

    serenity::semantic::SemanticAnalyzer analyzer(*parsed.program);
    auto checked = analyzer.analyze();
    if (!checked.ok()) {
        for (const auto& error : checked.errors) {
            std::cerr << error.location.line << ':' << error.location.column
                      << ": " << error.message << '\n';
        }
    }
    require(checked.ok(), "standard-library functions should pass semantic analysis");

    serenity::bytecode::BytecodeGenerator generator(*parsed.program, checked.model);
    auto generated = generator.generate();
    require(generated.ok(), "standard-library example should generate bytecode");

    std::ostringstream output;
    serenity::interpreter::Interpreter interpreter(*generated.module, output);
    const auto execution = interpreter.run();
    if (!execution.ok && execution.error.has_value()) {
        std::cerr << "runtime error: " << execution.error->message << '\n';
    }
    require(execution.ok, "standard-library example should execute in sninterp runtime");

    const std::string expected =
        "42\n"
        "5\n"
        "12\n"
        "100\n"
        "-1\n"
        "true\n"
        "true\n"
        "3.5\n"
        "1.25\n"
        "4.5\n"
        "12.75\n"
        "499500\n";
    require(output.str() == expected, "standard-library functions should produce expected results");

    std::cout << "All Serenity standard-library tests passed.\n";
    return EXIT_SUCCESS;
}
