#include "serenity/aot/llvm_backend.hpp"

#include <system_error>

#ifndef SERENITY_HAS_LLVM
#define SERENITY_HAS_LLVM 0
#endif

#if SERENITY_HAS_LLVM
#include <llvm/ADT/SmallVector.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>

#include <memory>
#include <optional>
#include <string_view>
#include <vector>
#endif

namespace serenity::aot {

#if SERENITY_HAS_LLVM
namespace {

[[nodiscard]] llvm::OptimizationLevel optimizationLevel(const std::string& value) {
    if (value == "-O1") return llvm::OptimizationLevel::O1;
    if (value == "-O2") return llvm::OptimizationLevel::O2;
    if (value == "-O3") return llvm::OptimizationLevel::O3;
    return llvm::OptimizationLevel::O0;
}

[[nodiscard]] llvm::CodeGenOptLevel codeGenOptimizationLevel(const std::string& value) {
    if (value == "-O1") return llvm::CodeGenOptLevel::Less;
    if (value == "-O2") return llvm::CodeGenOptLevel::Default;
    if (value == "-O3") return llvm::CodeGenOptLevel::Aggressive;
    return llvm::CodeGenOptLevel::None;
}

[[nodiscard]] std::string diagnosticText(const llvm::SMDiagnostic& diagnostic) {
    std::string message;
    llvm::raw_string_ostream stream(message);
    diagnostic.print("serenity", stream);
    stream.flush();
    return message;
}

[[nodiscard]] bool verifyModule(llvm::Module& module, std::string& error) {
    std::string verifierOutput;
    llvm::raw_string_ostream stream(verifierOutput);
    const bool invalid = llvm::verifyModule(module, &stream);
    stream.flush();
    if (invalid) {
        error = verifierOutput.empty() ? "LLVM module verification failed" : verifierOutput;
        return false;
    }
    return true;
}

[[nodiscard]] bool initializeNativeTarget(std::string& error) {
    if (llvm::InitializeNativeTarget()) {
        error = "LLVM could not initialize the native target";
        return false;
    }
    if (llvm::InitializeNativeTargetAsmPrinter()) {
        error = "LLVM could not initialize the native assembly printer";
        return false;
    }
    return true;
}

} // namespace
#endif

LLVMBackendResult LLVMBackend::emitObjectFile(
    const std::string& ir,
    const std::filesystem::path& output,
    const std::string& optimization) {
#if SERENITY_HAS_LLVM
    if (ir.empty()) return {false, "cannot compile an empty LLVM IR module"};
    if (optimization != "-O0" && optimization != "-O1" &&
        optimization != "-O2" && optimization != "-O3") {
        return {false, "unsupported optimization level '" + optimization + "'"};
    }

    std::string initializationError;
    if (!initializeNativeTarget(initializationError)) {
        return {false, std::move(initializationError)};
    }

    auto context = std::make_unique<llvm::LLVMContext>();
    const llvm::MemoryBufferRef buffer(llvm::StringRef(ir), "Serenity AOT module");
    llvm::SMDiagnostic parseDiagnostic;
    std::unique_ptr<llvm::Module> module =
        llvm::parseIR(buffer, parseDiagnostic, *context);
    if (!module) return {false, diagnosticText(parseDiagnostic)};

    const std::string tripleString = llvm::sys::getDefaultTargetTriple();
    const llvm::Triple triple(tripleString);
    std::string targetError;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, targetError);
    if (target == nullptr) {
        return {false, "cannot select native LLVM target: " + targetError};
    }

    llvm::TargetOptions targetOptions;
    std::unique_ptr<llvm::TargetMachine> targetMachine(target->createTargetMachine(
        triple, "generic", "", targetOptions, std::nullopt, std::nullopt,
        codeGenOptimizationLevel(optimization)));
    if (!targetMachine) {
        return {false, "LLVM failed to create a TargetMachine for '" + tripleString + "'"};
    }

    module->setTargetTriple(triple);
    module->setDataLayout(targetMachine->createDataLayout());

    std::string verificationError;
    if (!verifyModule(*module, verificationError)) {
        return {false, "invalid LLVM IR before optimization:\n" + verificationError};
    }

    if (optimization != "-O0") {
        llvm::LoopAnalysisManager loopAnalyses;
        llvm::FunctionAnalysisManager functionAnalyses;
        llvm::CGSCCAnalysisManager cgsccAnalyses;
        llvm::ModuleAnalysisManager moduleAnalyses;

        llvm::PassBuilder passBuilder(targetMachine.get());
        passBuilder.registerModuleAnalyses(moduleAnalyses);
        passBuilder.registerCGSCCAnalyses(cgsccAnalyses);
        passBuilder.registerFunctionAnalyses(functionAnalyses);
        passBuilder.registerLoopAnalyses(loopAnalyses);
        passBuilder.crossRegisterProxies(
            loopAnalyses, functionAnalyses, cgsccAnalyses, moduleAnalyses);

        llvm::ModulePassManager optimizationPipeline =
            passBuilder.buildPerModuleDefaultPipeline(optimizationLevel(optimization));
        optimizationPipeline.run(*module, moduleAnalyses);
    }

    if (!verifyModule(*module, verificationError)) {
        return {false, "invalid LLVM IR after optimization:\n" + verificationError};
    }

    std::error_code fileError;
    llvm::raw_fd_ostream objectOutput(output.string(), fileError, llvm::sys::fs::OF_None);
    if (fileError) {
        return {false, "cannot open object output '" + output.string() + "': " + fileError.message()};
    }

    llvm::legacy::PassManager codeGenerationPasses;
    if (targetMachine->addPassesToEmitFile(
            codeGenerationPasses, objectOutput, nullptr,
            llvm::CodeGenFileType::ObjectFile)) {
        return {false, "LLVM target does not support object-file emission"};
    }

    codeGenerationPasses.run(*module);
    objectOutput.flush();
    if (objectOutput.has_error()) {
        return {false, "failed while writing native object file '" + output.string() + "'"};
    }
    return {true, {}};
#else
    (void)ir;
    (void)output;
    (void)optimization;
    return {false, "Serenity AOT requires LLVM development libraries; reconfigure with LLVM_DIR set to Homebrew's LLVMConfig.cmake"};
#endif
}

LLVMBackendResult LLVMBackend::linkExecutable(
    const std::filesystem::path& objectFile,
    const std::filesystem::path& runtimeLibrary,
    const std::filesystem::path& output,
    const NativeLinkOptions& options) {
#if SERENITY_HAS_LLVM
#if defined(__APPLE__)
    if (!std::filesystem::exists(objectFile)) {
        return {false, "native object file does not exist: '" + objectFile.string() + "'"};
    }
    if (!std::filesystem::exists(runtimeLibrary)) {
        return {false, "Serenity runtime library does not exist: '" + runtimeLibrary.string() + "'"};
    }
    if (options.sdkRoot.empty() || !std::filesystem::exists(options.sdkRoot)) {
        return {false, "macOS SDK was not found; install Xcode Command Line Tools or configure CMAKE_OSX_SYSROOT"};
    }

    std::filesystem::path linker = options.linker;
    if (linker.empty()) {
        auto found = llvm::sys::findProgramByName("ld");
        if (!found) return {false, "could not locate the macOS system linker (ld)"};
        linker = *found;
    }

#if defined(__arm64__) || defined(__aarch64__)
    constexpr const char* architecture = "arm64";
#else
    constexpr const char* architecture = "x86_64";
#endif

    const std::string linkerPath = linker.string();
    const std::string objectPath = objectFile.string();
    const std::string runtimePath = runtimeLibrary.string();
    const std::string outputPath = output.string();
    const std::string sdkRoot = options.sdkRoot.string();
    const std::string minimumVersion = options.minimumMacOSVersion;
    const std::string sdkVersion = options.sdkVersion.empty() ? "15.0" : options.sdkVersion;

    // ExecuteAndWait takes argv, including argv[0]. Arguments are passed as
    // distinct strings; no shell is involved, so spaces and shell metacharacters
    // in file paths remain ordinary path characters.
    llvm::SmallVector<llvm::StringRef, 16> arguments;
    arguments.push_back(linkerPath);
    arguments.push_back("-arch");
    arguments.push_back(architecture);
    arguments.push_back("-platform_version");
    arguments.push_back("macos");
    arguments.push_back(minimumVersion);
    arguments.push_back(sdkVersion);
    arguments.push_back("-syslibroot");
    arguments.push_back(sdkRoot);
    arguments.push_back("-e");
    arguments.push_back("_main");
    arguments.push_back("-o");
    arguments.push_back(outputPath);
    arguments.push_back(objectPath);
    arguments.push_back(runtimePath);
    arguments.push_back("-lc++");
    arguments.push_back("-lSystem");

    std::string executionError;
    bool executionFailed = false;
    const int status = llvm::sys::ExecuteAndWait(
        linkerPath, arguments, std::nullopt, {}, 0, 0,
        &executionError, &executionFailed);
    if (executionFailed || status != 0) {
        std::string message = "native link failed";
        if (!executionError.empty()) message += ": " + executionError;
        if (!executionFailed) message += " (linker exit status " + std::to_string(status) + ")";
        return {false, std::move(message)};
    }
    return {true, {}};
#else
    (void)objectFile;
    (void)runtimeLibrary;
    (void)output;
    (void)options;
    return {false, "direct native executable linking is currently implemented for macOS; use --emit-object on this host"};
#endif
#else
    (void)objectFile;
    (void)runtimeLibrary;
    (void)output;
    (void)options;
    return {false, "Serenity AOT requires LLVM development libraries"};
#endif
}

} // namespace serenity::aot
