#include "serenity/lexer/lexer.hpp"
#include "serenity/parser/parser.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

serenity::parser::ParseResult parseSource(const std::string& source) {
    serenity::Lexer lexer(source);
    serenity::LexResult lexed = lexer.tokenize();
    require(lexed.ok(), "test source should tokenize without errors");
    serenity::parser::Parser parser(std::move(lexed.tokens));
    return parser.parse();
}

} // namespace

int main() {
    using namespace serenity;
    using namespace serenity::ast;
    using namespace serenity::parser;

    const std::string source = R"SERENITY(
struct {
    string name = "Unknown";
    int age;
} Person;

enum { North, South, East, West } Direction;

int globalCount;
mut string title = 'Serenity';

func add(int, int) => int;
add(a, b) => int {
    return a + b;
}

func multiply(int left, int right) => int {
    return left * right;
}

func main() => int {
    mut int age = 17;
    bool isAdult;
    string nothing = null;
    Person person = { .name = "Aegon", .age = 120 };

    if ((age + 1) >= 18 && !isAdult) {
        println("Adult");
    } else if (age == 17) {
        println("Seventeen");
    } else {
        println("Minor");
    }

    while (age < 20) {
        age += 1;
    }

    person.age = age;
    Direction direction = Direction.North;
    return add(age, 3);
}
)SERENITY";

    ParseResult parsed = parseSource(source);
    if (!parsed.ok()) {
        for (const ParseError& error : parsed.errors) {
            std::cerr << error.location.line << ':' << error.location.column
                      << ": " << error.message << '\n';
        }
    }
    require(parsed.ok(), "complete Serenity program should parse");
    require(parsed.program->declarations().size() == 8, "all top-level declarations should be retained");

    const auto* person = dynamic_cast<const StructDecl*>(parsed.program->declarations()[0].get());
    require(person != nullptr, "first declaration should be a struct");
    require(person->name() == "Person", "struct type name should be retained");
    require(person->fields().size() == 2, "struct fields should be retained");
    require(person->fields()[0].defaultValue != nullptr, "struct field default should be retained");

    const auto* direction = dynamic_cast<const EnumDecl*>(parsed.program->declarations()[1].get());
    require(direction != nullptr, "second declaration should be an enum");
    require(direction->members().size() == 4, "enum members should be retained");

    const auto* global = dynamic_cast<const GlobalVariableDecl*>(parsed.program->declarations()[2].get());
    require(global != nullptr, "third declaration should be a global variable");
    require(global->variable().initializer == nullptr, "uninitialized global should remain uninitialized in the AST");

    const auto* prototype = dynamic_cast<const FunctionDecl*>(parsed.program->declarations()[4].get());
    require(prototype != nullptr && prototype->isPrototype(), "function prototype should be recognized");
    require(prototype->parameters().size() == 2, "prototype parameter types should be retained");
    require(!prototype->parameters()[0].name.has_value(), "prototype parameter names may be omitted");

    const auto* signatureImplementation = dynamic_cast<const FunctionDecl*>(parsed.program->declarations()[5].get());
    require(signatureImplementation != nullptr, "signature implementation should be a function declaration");
    require(signatureImplementation->form() == FunctionForm::SignatureImplementation,
            "signature implementation form should be distinguished");
    require(!signatureImplementation->parameters()[0].type.has_value(),
            "signature implementation parameter types should be resolved later");

    const auto* mainFunction = dynamic_cast<const FunctionDecl*>(parsed.program->declarations()[7].get());
    require(mainFunction != nullptr && mainFunction->body() != nullptr,
            "main function definition should be retained");
    require(mainFunction->body()->statements().size() == 9,
            "main function statements should be retained");
    const auto* uninitializedBool = dynamic_cast<const VariableDeclStmt*>(
        mainFunction->body()->statements()[1].get());
    require(uninitializedBool != nullptr && uninitializedBool->variable().initializer == nullptr,
            "uninitialized bool declaration should remain uninitialized in the AST");
    const auto* explicitNull = dynamic_cast<const VariableDeclStmt*>(
        mainFunction->body()->statements()[2].get());
    require(explicitNull != nullptr && explicitNull->variable().initializer != nullptr,
            "explicit null initializer should be represented in the AST");
    require(dynamic_cast<const NullLiteralExpr*>(explicitNull->variable().initializer.get()) != nullptr,
            "null initializer should produce a NullLiteralExpr");

    // Verify conventional left-associative precedence: 1 + (2 * 3).
    ParseResult precedenceResult = parseSource(R"SERENITY(
func main() => int {
    return 1 + 2 * 3;
}
)SERENITY");
    require(precedenceResult.ok(), "precedence test should parse");
    const auto* precedenceFunction = dynamic_cast<const FunctionDecl*>(
        precedenceResult.program->declarations()[0].get());
    const auto* returnStatement = dynamic_cast<const ReturnStmt*>(
        precedenceFunction->body()->statements()[0].get());
    const auto* addition = dynamic_cast<const BinaryExpr*>(&returnStatement->value());
    require(addition != nullptr && addition->op() == BinaryOperator::Add,
            "addition should be the root expression");
    const auto* multiplication = dynamic_cast<const BinaryExpr*>(&addition->right());
    require(multiplication != nullptr && multiplication->op() == BinaryOperator::Multiply,
            "multiplication should bind more tightly than addition");

    // Executable statements are not allowed at top level.
    ParseResult topLevelStatement = parseSource("println(\"not allowed\");\n");
    require(!topLevelStatement.ok(), "top-level executable statement should be rejected");
    require(!topLevelStatement.errors.empty(), "top-level statement error should be reported");

    // `for` is reserved but intentionally not implemented in this version.
    ParseResult forLoop = parseSource(R"SERENITY(
func main() => int {
    for (;;) { }
    return 0;
}
)SERENITY");
    require(!forLoop.ok(), "for loops should be rejected until their syntax is specified");

    // Missing semicolons should produce a useful syntax error.
    ParseResult missingSemicolon = parseSource(R"SERENITY(
func main() => int {
    int value = 10
    return value;
}
)SERENITY");
    require(!missingSemicolon.ok(), "missing semicolon should be diagnosed");

    std::cout << "All Serenity parser tests passed.\n";
    return EXIT_SUCCESS;
}
