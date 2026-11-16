#include "serenity/aot/llvm_backend.hpp"

#include <cstdlib>
#include <filesystem>
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
    const std::filesystem::path output =
        std::filesystem::temp_directory_path() / "serenity_llvm_backend_test.o";
    std::error_code error;
    std::filesystem::remove(output, error);

    const std::string ir = R"IR(
        define i32 @main() {
        entry:
            ret i32 0
        }
    )IR";

    const auto result = serenity::aot::LLVMBackend::emitObjectFile(ir, output, "-O2");
    if (!result.ok()) {
        std::cerr << "FAIL: direct LLVM object generation failed: " << result.message << '\n';
        return EXIT_FAILURE;
    }

    require(std::filesystem::exists(output), "LLVM TargetMachine should create an object file");
    require(std::filesystem::file_size(output) > 0, "generated object file should not be empty");

    std::filesystem::remove(output, error);
    std::cout << "All Serenity direct LLVM backend tests passed.\n";
    return EXIT_SUCCESS;
}
