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

#ifndef SERENITY_IO_TEST_SOURCE
#define SERENITY_IO_TEST_SOURCE ""
#endif

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
}

int main() {
    const std::filesystem::path sourcePath{SERENITY_IO_TEST_SOURCE};
    require(!sourcePath.empty(), "I/O test source path must be configured");

    serenity::Preprocessor preprocessor;
    const auto preprocessed = preprocessor.preprocessFile(sourcePath);
    require(preprocessed.ok(), "std/io.snh should preprocess");

    serenity::Lexer lexer(preprocessed.source);
    auto lexed = lexer.tokenize();
    require(lexed.ok(), "I/O example should tokenize");

    serenity::parser::Parser parser(std::move(lexed.tokens));
    auto parsed = parser.parse();
    require(parsed.ok(), "I/O example should parse");

    serenity::semantic::SemanticAnalyzer analyzer(*parsed.program);
    auto checked = analyzer.analyze();
    if (!checked.ok()) {
        for (const auto& error : checked.errors) std::cerr << error.message << '\n';
    }
    require(checked.ok(), "I/O primitives and stdlib wrappers should type-check");

    serenity::bytecode::BytecodeGenerator generator(*parsed.program, checked.model);
    auto generated = generator.generate();
    require(generated.ok(), "I/O example should generate bytecode");

    std::istringstream input("Aegon Targaryen\n");
    std::ostringstream output;
    std::ostringstream error;
    serenity::interpreter::Interpreter interpreter(*generated.module, input, output, error);
    const auto execution = interpreter.run();
    if (!execution.ok && execution.error.has_value()) {
        std::cerr << execution.error->message << '\n';
    }
    require(execution.ok, "I/O example should execute");
    require(output.str() == "Name: Aegon Targaryen\n42\n", "stdout contents should match");
    require(error.str() == "diagnostic\n", "stderr contents should match");

    std::cout << "All Serenity I/O tests passed.\n";
    return EXIT_SUCCESS;
}
