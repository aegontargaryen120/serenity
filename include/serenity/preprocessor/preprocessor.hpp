#pragma once

#include "serenity/lexer/token.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace serenity {

enum class PreprocessSeverity {
    Warning,
    Error,
};

struct SourceOrigin {
    std::filesystem::path file;
    SourceLocation location;
};

// Maps a half-open range in the expanded source back to the original file.
// Each segment is normally one physical source line, keeping the mapping much
// smaller than a one-entry-per-character map.
struct SourceMapEntry {
    std::size_t expandedBegin {0};
    std::size_t expandedEnd {0};
    std::filesystem::path sourceFile;
    std::size_t sourceOffset {0};
    std::size_t sourceLine {1};
    std::size_t sourceColumn {1};
};

struct PreprocessDiagnostic {
    PreprocessSeverity severity {PreprocessSeverity::Error};
    std::string message;
    SourceOrigin origin;
};

struct PreprocessResult {
    std::string source;
    std::vector<SourceMapEntry> sourceMap;
    std::vector<PreprocessDiagnostic> diagnostics;

    [[nodiscard]] bool ok() const noexcept;
    [[nodiscard]] std::optional<SourceOrigin> originAt(std::size_t expandedOffset) const;
};

struct PreprocessorOptions {
    // Protects against pathological include chains. Circular includes are
    // separately detected and skipped using automatic header guards.
    std::size_t maxIncludeDepth {256};

    // Optional Serenity standard-library root. User-relative includes are
    // searched first; unresolved relative paths are then searched beneath this
    // directory. When empty, SERENITY_STDLIB_PATH or the build-time default is
    // used. This is not a general-purpose -I include path.
    std::filesystem::path standardLibraryDirectory;
};

class Preprocessor {
public:
    explicit Preprocessor(PreprocessorOptions options = {});

    // Reads and preprocesses a source file. Include paths are resolved relative
    // to the directory containing the file that owns the %include directive.
    [[nodiscard]] PreprocessResult preprocessFile(const std::filesystem::path& sourcePath);

    // Preprocesses in-memory source. sourcePath supplies the filename and base
    // directory used for relative includes (use an absolute path when possible).
    [[nodiscard]] PreprocessResult preprocessSource(
        std::string source,
        const std::filesystem::path& sourcePath = "<memory>");

private:
    enum class LexicalState {
        Normal,
        BlockComment,
        String,
    };

    struct IncludeDirective {
        bool valid {false};
        std::string path;
        std::string error;
    };

    PreprocessorOptions options_;
    PreprocessResult result_;
    std::vector<std::filesystem::path> activeIncludes_;
    std::vector<std::filesystem::path> includedHeaders_;

    void reset();
    void processFile(const std::filesystem::path& path, std::string source, std::size_t depth);
    void processSourceText(
        std::string_view source,
        const std::filesystem::path& sourcePath,
        std::size_t depth);

    void appendMapped(
        std::string_view text,
        const std::filesystem::path& sourcePath,
        std::size_t sourceOffset,
        std::size_t sourceLine,
        std::size_t sourceColumn);

    void addDiagnostic(
        PreprocessSeverity severity,
        std::string message,
        const std::filesystem::path& sourcePath,
        SourceLocation location);

    [[nodiscard]] IncludeDirective parseInclude(std::string_view line) const;
    [[nodiscard]] std::optional<std::filesystem::path> resolveInclude(
        std::string_view includePath,
        const std::filesystem::path& includingFile,
        const SourceOrigin& directiveOrigin);

    [[nodiscard]] static std::filesystem::path normalizedPath(const std::filesystem::path& path);
    [[nodiscard]] static bool isHeaderPath(const std::filesystem::path& path);
    [[nodiscard]] static bool isInPathList(
        const std::vector<std::filesystem::path>& paths,
        const std::filesystem::path& path);
    static void updateLexicalState(std::string_view line, LexicalState& state, char& stringQuote);
    [[nodiscard]] static bool hasLineContinuation(std::string_view line);
};

} // namespace serenity
