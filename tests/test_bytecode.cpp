#include "serenity/bytecode/generator.hpp"
#include "serenity/lexer/lexer.hpp"
#include "serenity/parser/parser.hpp"
#include "serenity/semantic/analyzer.hpp"

#include <algorithm>
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

struct CheckedProgram {
    std::unique_ptr<serenity::ast::Program> program;
    serenity::semantic::SemanticModel semanticModel;
};

CheckedProgram parseAndCheck(const std::string& source) {
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

    return {std::move(parsed.program), std::move(checked.model)};
}

bool hasOpcode(
    const serenity::bytecode::Function& function,
    serenity::bytecode::Opcode opcode) {
    for (const auto& instruction : function.code) {
        if (instruction.opcode == opcode) return true;
    }
    return false;
}

} // namespace

int main() {
    using namespace serenity::bytecode;
    static_assert(sizeof(Instruction) == 12);

    const std::string source = R"SERENITY(
struct {
    string name = "Unknown";
    int age = 0;
} Person;

enum { North, South, East, West } Direction;

struct {
    Direction facing = Direction.North;
} Marker;

int globalCount;
mut int globalSeed = 10;

func add(int, int) => int;
add(a, b) => int {
    return a + b;
}

func main() => int {
    mut int index = 0;
    mut Person person = { .name = "Aegon", .age = 120 };
    person.age += 1;
    Marker marker = {};

    bool first = true;
    bool shouldRun = first && (index == 0);

    if (shouldRun) {
        index = add(index, globalSeed);
    } else {
        index = 3;
    }

    while (index < 14) {
        index += 1;
        if (index == 13) {
            break;
        }
    }

    Direction direction = Direction.North;
    println(person.name);
    println(person.age);
    println(index);
    return 0;
}
)SERENITY";

    CheckedProgram checked = parseAndCheck(source);
    BytecodeGenerator generator(*checked.program, checked.semanticModel);
    BytecodeResult generated = generator.generate();
    if (!generated.ok()) {
        for (const auto& error : generated.errors) {
            std::cerr << error.location.line << ':' << error.location.column
                      << ": bytecode: " << error.message << '\n';
        }
    }
    require(generated.ok(), "semantically valid program should generate bytecode");

    const BytecodeModule& module = *generated.module;
    require(module.versionMajor == 0 && module.versionMinor == 6,
            "module should carry bytecode version 0.6");
    require(module.globals.size() == 2, "global variables should be represented in the module");
    require(module.structs.size() == 2,
            "all struct layouts should be represented in the module");
    const auto personDescriptor = std::find_if(module.structs.begin(), module.structs.end(),
        [](const StructDescriptor& structure) { return structure.name == "Person"; });
    require(personDescriptor != module.structs.end() && personDescriptor->fields.size() == 2,
            "Person field layout should be represented in the module");
    require(personDescriptor->fields[0].defaultConstant.has_value(),
            "literal struct field defaults should be stored in the constant pool");
    const auto markerDescriptor = std::find_if(module.structs.begin(), module.structs.end(),
        [](const StructDescriptor& structure) { return structure.name == "Marker"; });
    require(markerDescriptor != module.structs.end() && markerDescriptor->fields[0].defaultConstant.has_value(),
            "enum member defaults should be stored in the constant pool");
    require(module.enums.size() == 1 && module.enums[0].members.size() == 4,
            "enum members should be represented in the module");
    require(module.entryFunction.has_value(), "main should be recorded as the entry function");
    require(module.globalInitializerFunction.has_value(),
            "global defaults and initializers should have an initialization function");

    const auto mainIt = std::find_if(module.functions.begin(), module.functions.end(),
        [](const Function& function) { return function.name == "main"; });
    require(mainIt != module.functions.end(), "main function should be emitted");
    require(hasOpcode(*mainIt, Opcode::Call), "function calls should emit CALL");
    require(hasOpcode(*mainIt, Opcode::JumpIfFalse), "if/while should emit conditional jumps");
    require(hasOpcode(*mainIt, Opcode::Jump), "loops and branches should emit jumps");
    require(hasOpcode(*mainIt, Opcode::NewStruct), "struct initializers should emit NEW_STRUCT");
    require(hasOpcode(*mainIt, Opcode::GetField), "struct field reads should emit GET_FIELD");
    require(hasOpcode(*mainIt, Opcode::SetField), "struct field writes should emit SET_FIELD");
    require(hasOpcode(*mainIt, Opcode::PushEnum), "enum members should emit PUSH_ENUM");
    require(hasOpcode(*mainIt, Opcode::Println), "println should emit PRINTLN");
    require(hasOpcode(*mainIt, Opcode::Duplicate), "field initializers should preserve their struct value with DUP");
    require(mainIt->code.size() == mainIt->debugLocations.size(),
            "every instruction should have a matching source span");
    require(mainIt->locals.size() >= 4, "parameters and local variables should receive local slots");

    const auto addIt = std::find_if(module.functions.begin(), module.functions.end(),
        [](const Function& function) { return function.name == "add"; });
    require(addIt != module.functions.end(), "signature implementation should generate a function");
    require(addIt->parameterTypes.size() == 2, "prototype parameter types should be applied to implementation");
    require(addIt->locals.size() == 2, "function parameters should occupy initial local slots");

    const Function& initializer = module.functions[*module.globalInitializerFunction];
    require(initializer.synthetic, "global initializer should be marked synthetic");
    require(hasOpcode(initializer, Opcode::StoreGlobal), "global initializer should initialize global slots");
    require(hasOpcode(initializer, Opcode::Return), "global initializer should return to its caller");

    const std::string listing = disassemble(module);
    require(listing.find("PUSH_ENUM") != std::string::npos, "disassembler should name instructions");
    require(listing.find("__serenity_init_globals") != std::string::npos,
            "disassembler should include the synthetic global initializer");

    // Logical operators must compile with branches so they short-circuit.
    const std::string shortCircuitSource = R"SERENITY(
func truth() => bool { return true; }
func main() => int {
    bool value = false && truth();
    return 0;
}
)SERENITY";
    CheckedProgram shortCircuitChecked = parseAndCheck(shortCircuitSource);
    BytecodeGenerator shortCircuitGenerator(*shortCircuitChecked.program, shortCircuitChecked.semanticModel);
    auto shortCircuit = shortCircuitGenerator.generate();
    require(shortCircuit.ok(), "short-circuit test should generate bytecode");
    const auto shortMain = std::find_if(shortCircuit.module->functions.begin(), shortCircuit.module->functions.end(),
        [](const Function& function) { return function.name == "main"; });
    require(shortMain != shortCircuit.module->functions.end(), "short-circuit main should exist");
    require(hasOpcode(*shortMain, Opcode::JumpIfFalse), "logical AND should use conditional branching");

    // Missing function implementations cannot be represented in a standalone
    // bytecode module yet; report this at generation rather than producing a
    // dangling function index for the interpreter.
    const std::string unresolvedSource = R"SERENITY(
func external(int) => int;
func main() => int { return 0; }
)SERENITY";
    CheckedProgram unresolvedChecked = parseAndCheck(unresolvedSource);
    BytecodeGenerator unresolvedGenerator(*unresolvedChecked.program, unresolvedChecked.semanticModel);
    auto unresolved = unresolvedGenerator.generate();
    require(!unresolved.ok(), "unimplemented prototypes should be rejected by standalone bytecode generation");

    std::cout << "All Serenity bytecode generation tests passed.\n";
    return EXIT_SUCCESS;
}
