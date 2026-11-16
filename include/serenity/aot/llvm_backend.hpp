#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace serenity::aot {

struct LLVMBackendResult {
    bool success {false};
    std::string message;

    [[nodiscard]] bool ok() const noexcept { return success; }
};

struct NativeLinkOptions {
    std::filesystem::path linker;
    std::filesystem::path sdkRoot;
    std::string sdkVersion;
    std::string minimumMacOSVersion {"13.0"};
};

// Compiles Serenity's generated textual LLVM IR to a native object file using
// LLVM's C++ APIs. No compiler driver is involved in parsing, optimization, or
// machine-code generation.
class LLVMBackend {
public:
    [[nodiscard]] static LLVMBackendResult emitObjectFile(
        const std::string& ir,
        const std::filesystem::path& output,
        const std::string& optimizationLevel);

    // Links an LLVM-generated object file and Serenity's C-ABI runtime archive.
    // On macOS this invokes the platform linker directly, not clang/clang++.
    [[nodiscard]] static LLVMBackendResult linkExecutable(
        const std::filesystem::path& objectFile,
        const std::filesystem::path& runtimeLibrary,
        const std::filesystem::path& output,
        const NativeLinkOptions& options);
};

} // namespace serenity::aot
