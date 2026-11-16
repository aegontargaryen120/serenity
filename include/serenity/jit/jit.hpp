#pragma once

#include "serenity/bytecode/module.hpp"
#include "serenity/interpreter/interpreter.hpp"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>

namespace serenity::jit {

struct JitOptions {
    std::uint64_t hotLoopThreshold {8};
    std::uint64_t maxInstructions {100'000'000};
    std::size_t maxCallDepth {1024};
    bool showStatistics {false};
};

struct JitStatistics {
    std::uint64_t observedBackedges {0};
    std::uint64_t recordingAttempts {0};
    std::uint64_t compiledTraces {0};
    std::uint64_t rejectedTraces {0};
    std::uint64_t traceExecutions {0};
    std::uint64_t traceIterations {0};
    std::uint64_t nativeBytecodeInstructions {0};
    std::uint64_t sideExits {0};
    std::uint64_t budgetExits {0};
    std::uint64_t deoptimizations {0};
    std::uint64_t compilationMicroseconds {0};
    std::string lastTraceDiagnostic;
};

struct JitResult {
    interpreter::ExecutionResult execution;
    JitStatistics jit;
    bool jitAvailable {false};
    std::string initializationError;
};

// A tracing JIT that observes interpreter bytecode execution, records hot
// backedge-to-backedge traces, specializes integer/boolean operations, and
// compiles those traces to native code with LLVM ORC. Unsupported loops remain
// in the bytecode interpreter.
class TracingJIT {
public:
    explicit TracingJIT(const bytecode::BytecodeModule& module, JitOptions options = {});
    ~TracingJIT();

    TracingJIT(const TracingJIT&) = delete;
    TracingJIT& operator=(const TracingJIT&) = delete;
    TracingJIT(TracingJIT&&) noexcept;
    TracingJIT& operator=(TracingJIT&&) noexcept;

    [[nodiscard]] JitResult run(std::ostream& output);
    [[nodiscard]] static bool isAvailable() noexcept;
    [[nodiscard]] static std::string llvmVersion();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string formatStatistics(const JitStatistics& statistics);

} // namespace serenity::jit
