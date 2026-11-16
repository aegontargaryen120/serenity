#include "serenity/aot/llvm_emitter.hpp"
#include "serenity/bytecode/instruction.hpp"
#include "serenity/bytecode/module.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
}

int main() {
    using namespace serenity::bytecode;
    BytecodeModule module;
    module.constants.push_back(Constant{ValueType::Int, std::int64_t{0}});
    Function mainFunction;
    mainFunction.name = "main";
    mainFunction.returnType = TypeInfo{ValueType::Int, {}};
    mainFunction.code = {
        Instruction{Opcode::PushConstant, ValueType::Invalid, 0, 0, 0},
        Instruction{Opcode::Return, ValueType::Int, 0, 0, 0},
    };
    mainFunction.debugLocations.resize(mainFunction.code.size());
    module.functions.push_back(std::move(mainFunction));
    module.entryFunction = 0;

    serenity::aot::LLVMIREmitter emitter(module);
    const auto result = emitter.emit();
    require(result.ok(), "valid minimal bytecode module should lower to LLVM IR");
    require(result.ir->find("define internal void @sn_fn_0") != std::string::npos,
            "LLVM IR should contain a native Serenity function");
    require(result.ir->find("define i32 @main()") != std::string::npos,
            "LLVM IR should contain a native process entry point");
    require(result.ir->find("sn_rt_make") != std::string::npos,
            "LLVM IR should initialize the constant through the runtime ABI");

    BytecodeModule invalid;
    serenity::aot::LLVMIREmitter invalidEmitter(invalid);
    const auto invalidResult = invalidEmitter.emit();
    require(!invalidResult.ok(), "module without main must be rejected");
    require(!invalidResult.errors.empty(), "missing-main diagnostic should be available");

    std::cout << "All Serenity AOT emitter tests passed.\n";
    return EXIT_SUCCESS;
}
