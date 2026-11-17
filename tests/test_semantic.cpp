#include "serenity/lexer/lexer.hpp"
#include "serenity/parser/parser.hpp"
#include "serenity/semantic/analyzer.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

serenity::ast::Program parseProgram(const std::string& source) {
    serenity::Lexer lexer(source);
    serenity::LexResult lexed = lexer.tokenize();
    require(lexed.ok(), "semantic test source should tokenize");

    serenity::parser::Parser parser(std::move(lexed.tokens));
    serenity::parser::ParseResult parsed = parser.parse();
    if (!parsed.ok()) {
        for (const auto& error : parsed.errors) {
            std::cerr << error.location.line << ':' << error.location.column
                      << ": parser: " << error.message << '\n';
        }
    }
    require(parsed.ok(), "semantic test source should parse");
    return std::move(*parsed.program);
}

bool containsMessage(
    const serenity::semantic::SemanticResult& result,
    const std::string& fragment) {
    for (const auto& error : result.errors) {
        if (error.message.find(fragment) != std::string::npos) return true;
    }
    return false;
}

} // namespace

int main() {
    using namespace serenity;

    const std::string validSource = R"SERENITY(
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
    mut Person person = { .name = "Aegon", .age = 120 };
    Person emptyPerson;
    Direction direction = Direction.North;
    Direction noDirection = null;
    float ratio = 1.25;
    double precise = 2.5;

    if ((age + 1) >= 18 && !isAdult) {
        println("Adult");
    } else if (age == 17) {
        println("Seventeen");
    } else {
        println("Minor");
    }

    while (age < 20) {
        age += 1;
        if (age == 19) {
            continue;
        }
    }

    mut int total = 0;
    for (mut int index = 0; index < 5; index += 1) {
        if (index == 2) {
            continue;
        }
        total += index;
    }

    person.age = age;
    println(add(age, 3));
    println(ratio + 0.5);
    println(precise);
    return multiply(age, globalCount);
}
)SERENITY";

    ast::Program validProgram = parseProgram(validSource);
    semantic::SemanticAnalyzer validAnalyzer(validProgram);
    semantic::SemanticResult valid = validAnalyzer.analyze();
    if (!valid.ok()) {
        for (const auto& error : valid.errors) {
            std::cerr << error.location.line << ':' << error.location.column
                      << ": semantic: " << error.message << '\n';
        }
    }
    require(valid.ok(), "well-typed Serenity program should pass semantic analysis");
    require(!valid.model.expressionTypes.empty(), "semantic model should retain inferred expression types");

    const std::string invalidSource = R"SERENITY(
struct { int value; } Box;

enum { Red, Green } Colour;

func identity(int value) => int {
    return value;
}

func main() => int {
    int fixed = 1;
    fixed = 2;
    mut int number = "not a number";
    bool condition = 42;
    Box box = { .missing = 1 };
    Colour colour = Colour.Blue;
    Colour.Red = 2;
    int badNull = null;
    if (number) {
        println("wrong condition");
    }
    identity("wrong argument type");
    identity();
    for (mut int index = 0; index; index += 1) {
        continue;
    }
    break;
    return "wrong return type";
}
)SERENITY";

    ast::Program invalidProgram = parseProgram(invalidSource);
    semantic::SemanticAnalyzer invalidAnalyzer(invalidProgram);
    semantic::SemanticResult invalid = invalidAnalyzer.analyze();
    require(!invalid.ok(), "ill-typed Serenity program should fail semantic analysis");
    require(containsMessage(invalid, "immutable variable"), "immutable assignment should be diagnosed");
    require(containsMessage(invalid, "initializer for variable 'number'"), "initializer type mismatch should be diagnosed");
    require(containsMessage(invalid, "initializer for variable 'condition'"), "bool type mismatch should be diagnosed");
    require(containsMessage(invalid, "has no field 'missing'"), "unknown struct field should be diagnosed");
    require(containsMessage(invalid, "has no member 'Blue'"), "unknown enum member should be diagnosed");
    require(containsMessage(invalid, "enum members are values and cannot be assigned to"),
            "enum members should not be assignable");
    require(containsMessage(invalid, "initializer for variable 'badNull'"), "null-to-int assignment should be diagnosed");
    require(containsMessage(invalid, "if condition must have type 'bool'"), "non-bool condition should be diagnosed");
    require(containsMessage(invalid, "argument 1 of 'identity'"), "function argument type mismatch should be diagnosed");
    require(containsMessage(invalid, "expects 1 argument(s), but received 0"), "function argument count mismatch should be diagnosed");
    require(containsMessage(invalid, "for condition must have type 'bool'"),
            "non-bool for condition should be diagnosed");
    require(containsMessage(invalid, "'break' is only valid inside a loop"), "break outside loop should be diagnosed");
    require(containsMessage(invalid, "return value has type 'string'"), "return type mismatch should be diagnosed");

    const std::string immutableStructSource = R"SERENITY(
struct { int age; } Person;
func main() => int {
    Person person = { .age = 17 };
    person.age = 18;
    return 0;
}
)SERENITY";
    ast::Program immutableStructProgram = parseProgram(immutableStructSource);
    semantic::SemanticAnalyzer immutableStructAnalyzer(immutableStructProgram);
    semantic::SemanticResult immutableStruct = immutableStructAnalyzer.analyze();
    require(!immutableStruct.ok(), "fields of an immutable struct binding should not be mutable");
    require(containsMessage(immutableStruct, "cannot modify fields of immutable variable 'person'"),
            "immutable struct field assignment should be diagnosed");

    const std::string mutableStructSource = R"SERENITY(
struct { int age; } Person;
func main() => int {
    mut Person person = { .age = 17 };
    person.age = 18;
    return 0;
}
)SERENITY";
    ast::Program mutableStructProgram = parseProgram(mutableStructSource);
    semantic::SemanticAnalyzer mutableStructAnalyzer(mutableStructProgram);
    semantic::SemanticResult mutableStruct = mutableStructAnalyzer.analyze();
    require(mutableStruct.ok(), "fields of a mutable struct binding should be mutable");

    const std::string forScopeSource = R"SERENITY(
func main() => int {
    for (mut int index = 0; index < 2; index += 1) {
    }
    return index;
}
)SERENITY";
    ast::Program forScopeProgram = parseProgram(forScopeSource);
    semantic::SemanticAnalyzer forScopeAnalyzer(forScopeProgram);
    semantic::SemanticResult forScope = forScopeAnalyzer.analyze();
    require(!forScope.ok(), "for initializer variables must not escape the loop scope");
    require(containsMessage(forScope, "unknown variable 'index'"),
            "using a for initializer variable after the loop should be diagnosed");

    const std::string functionErrorsSource = R"SERENITY(
func add(int, int) => int;
add(a, b) => int {
    return a + b;
}

func add(float, float) => float;

func main() => int {
    return add(1, 2.0);
}
)SERENITY";

    ast::Program functionErrorsProgram = parseProgram(functionErrorsSource);
    semantic::SemanticAnalyzer functionErrorsAnalyzer(functionErrorsProgram);
    semantic::SemanticResult functionErrors = functionErrorsAnalyzer.analyze();
    require(!functionErrors.ok(), "overloading should be rejected in the initial language version");
    require(containsMessage(functionErrors, "function overloading is not supported"),
            "function overloading diagnostic should be present");

    std::cout << "All Serenity semantic analysis tests passed.\n";
    return EXIT_SUCCESS;
}
