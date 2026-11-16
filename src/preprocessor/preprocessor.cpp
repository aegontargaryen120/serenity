#include "serenity/preprocessor/preprocessor.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace serenity {
namespace {

[[nodiscard]] bool isHorizontalWhitespace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\f' || c == '\v';
}

[[nodiscard]] bool isIdentifierCharacter(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '_';
}

[[nodiscard]] std::string pathForMessage(const std::filesystem::path& path) {
    return path.generic_string();
}

} // namespace

bool PreprocessResult::ok() const noexcept {
    return std::none_of(diagnostics.begin(), diagnostics.end(), [](const PreprocessDiagnostic& diagnostic) {
        return diagnostic.severity == PreprocessSeverity::Error;
    });
}

std::optional<SourceOrigin> PreprocessResult::originAt(std::size_t expandedOffset) const {
    const auto it = std::upper_bound(
        sourceMap.begin(),
        sourceMap.end(),
        expandedOffset,
        [](std::size_t offset, const SourceMapEntry& entry) {
            return offset < entry.expandedBegin;
        });

    if (it == sourceMap.begin()) {
        return std::nullopt;
    }

    const SourceMapEntry& entry = *std::prev(it);
    if (expandedOffset < entry.expandedBegin || expandedOffset >= entry.expandedEnd) {
        return std::nullopt;
    }

    const std::size_t delta = expandedOffset - entry.expandedBegin;
    return SourceOrigin {
        entry.sourceFile,
        SourceLocation {
            entry.sourceOffset + delta,
            entry.sourceLine,
            entry.sourceColumn + delta,
        },
    };
}

Preprocessor::Preprocessor(PreprocessorOptions options)
    : options_(options) {
}

PreprocessResult Preprocessor::preprocessFile(const std::filesystem::path& sourcePath) {
    reset();

    const std::filesystem::path normalized = normalizedPath(sourcePath);
    std::ifstream input(normalized, std::ios::binary);
    if (!input) {
        addDiagnostic(
            PreprocessSeverity::Error,
            "unable to open source file '" + pathForMessage(normalized) + "'",
            normalized,
            SourceLocation {});
        return std::move(result_);
    }

    std::string source {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    if (input.bad()) {
        addDiagnostic(
            PreprocessSeverity::Error,
            "failed while reading source file '" + pathForMessage(normalized) + "'",
            normalized,
            SourceLocation {});
        return std::move(result_);
    }

    processFile(normalized, std::move(source), 0);
    return std::move(result_);
}

PreprocessResult Preprocessor::preprocessSource(
    std::string source,
    const std::filesystem::path& sourcePath) {
    reset();

    std::filesystem::path normalized = sourcePath;
    if (sourcePath == "<memory>") {
        normalized = std::filesystem::current_path() / "<memory>";
    } else {
        normalized = normalizedPath(sourcePath);
    }

    processFile(normalized, std::move(source), 0);
    return std::move(result_);
}

void Preprocessor::reset() {
    result_ = {};
    activeIncludes_.clear();
    includedHeaders_.clear();
}

void Preprocessor::processFile(
    const std::filesystem::path& path,
    std::string source,
    std::size_t depth) {
    const std::filesystem::path normalized = normalizedPath(path);

    if (depth > options_.maxIncludeDepth) {
        addDiagnostic(
            PreprocessSeverity::Error,
            "maximum include depth of " + std::to_string(options_.maxIncludeDepth) + " exceeded",
            normalized,
            SourceLocation {});
        return;
    }

    activeIncludes_.push_back(normalized);
    if (isHeaderPath(normalized) && !isInPathList(includedHeaders_, normalized)) {
        includedHeaders_.push_back(normalized);
    }

    processSourceText(source, normalized, depth);
    activeIncludes_.pop_back();
}

void Preprocessor::processSourceText(
    std::string_view source,
    const std::filesystem::path& sourcePath,
    std::size_t depth) {
    LexicalState lexicalState = LexicalState::Normal;
    char stringQuote = '\0';
    std::size_t offset = 0;
    std::size_t lineNumber = 1;
    bool continuedFromPreviousLine = false;

    while (offset < source.size()) {
        const std::size_t lineStart = offset;
        const std::size_t contentEnd = source.find_first_of("\r\n", offset);
        const std::size_t actualContentEnd = contentEnd == std::string_view::npos
            ? source.size()
            : contentEnd;

        std::size_t lineEnd = actualContentEnd;
        if (contentEnd != std::string_view::npos) {
            if (source[contentEnd] == '\r' && contentEnd + 1 < source.size()
                && source[contentEnd + 1] == '\n') {
                lineEnd = contentEnd + 2;
            } else {
                lineEnd = contentEnd + 1;
            }
        }

        const std::string_view content = source.substr(lineStart, actualContentEnd - lineStart);
        const std::string_view lineEnding = source.substr(actualContentEnd, lineEnd - actualContentEnd);
        std::size_t firstNonWhitespace = 0;
        while (firstNonWhitespace < content.size() && isHorizontalWhitespace(content[firstNonWhitespace])) {
            ++firstNonWhitespace;
        }

        const bool canStartDirective = lexicalState == LexicalState::Normal
            && !continuedFromPreviousLine
            && !hasLineContinuation(content)
            && firstNonWhitespace < content.size()
            && content[firstNonWhitespace] == '%';

        if (canStartDirective) {
            const std::string_view directiveText = content.substr(firstNonWhitespace);
            const SourceOrigin directiveOrigin {
                sourcePath,
                SourceLocation {lineStart + firstNonWhitespace, lineNumber, firstNonWhitespace + 1},
            };
            const IncludeDirective directive = parseInclude(directiveText);

            if (!directive.valid) {
                addDiagnostic(
                    PreprocessSeverity::Error,
                    directive.error,
                    sourcePath,
                    directiveOrigin.location);
            } else {
                const auto includePath = resolveInclude(directive.path, sourcePath, directiveOrigin);
                if (includePath.has_value()) {
                    const std::filesystem::path resolved = *includePath;
                    if (isInPathList(activeIncludes_, resolved)) {
                        addDiagnostic(
                            PreprocessSeverity::Warning,
                            "circular include of '" + pathForMessage(resolved) + "' was skipped by the automatic header guard",
                            sourcePath,
                            directiveOrigin.location);
                    } else if (!isInPathList(includedHeaders_, resolved)) {
                        std::ifstream input(resolved, std::ios::binary);
                        if (!input) {
                            addDiagnostic(
                                PreprocessSeverity::Error,
                                "unable to open included header '" + pathForMessage(resolved) + "'",
                                sourcePath,
                                directiveOrigin.location);
                        } else {
                            std::string includedSource {
                                std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>()};
                            if (input.bad()) {
                                addDiagnostic(
                                    PreprocessSeverity::Error,
                                    "failed while reading included header '" + pathForMessage(resolved) + "'",
                                    sourcePath,
                                    directiveOrigin.location);
                            } else {
                                // Mark before processing so recursive inclusion is guarded.
                                includedHeaders_.push_back(resolved);
                                processFile(resolved, std::move(includedSource), depth + 1);
                            }
                        }
                    }
                }
            }

            // Keep the directive's physical newline. Besides making expansion
            // easier to inspect, this keeps following source text separated.
            if (!lineEnding.empty()) {
                appendMapped(
                    lineEnding,
                    sourcePath,
                    actualContentEnd,
                    lineNumber,
                    content.size() + 1);
            }
            continuedFromPreviousLine = false;
        } else {
            appendMapped(
                source.substr(lineStart, lineEnd - lineStart),
                sourcePath,
                lineStart,
                lineNumber,
                1);
            updateLexicalState(content, lexicalState, stringQuote);
            continuedFromPreviousLine = lexicalState == LexicalState::String
                || (lexicalState != LexicalState::BlockComment && hasLineContinuation(content));
        }

        offset = lineEnd;
        ++lineNumber;
    }

    // Empty files and a final line without a newline need no synthetic output.
}

void Preprocessor::appendMapped(
    std::string_view text,
    const std::filesystem::path& sourcePath,
    std::size_t sourceOffset,
    std::size_t sourceLine,
    std::size_t sourceColumn) {
    if (text.empty()) {
        return;
    }

    const std::size_t begin = result_.source.size();
    result_.source.append(text);
    result_.sourceMap.push_back(SourceMapEntry {
        begin,
        result_.source.size(),
        sourcePath,
        sourceOffset,
        sourceLine,
        sourceColumn,
    });
}

void Preprocessor::addDiagnostic(
    PreprocessSeverity severity,
    std::string message,
    const std::filesystem::path& sourcePath,
    SourceLocation location) {
    result_.diagnostics.push_back(PreprocessDiagnostic {
        severity,
        std::move(message),
        SourceOrigin {sourcePath, location},
    });
}

Preprocessor::IncludeDirective Preprocessor::parseInclude(std::string_view line) const {
    constexpr std::string_view keyword = "%include";
    if (!line.starts_with(keyword)
        || (line.size() > keyword.size() && isIdentifierCharacter(line[keyword.size()]))) {
        std::size_t end = 1;
        while (end < line.size() && isIdentifierCharacter(line[end])) {
            ++end;
        }
        return {false, {}, "unknown preprocessor directive '" + std::string(line.substr(0, end)) + "'"};
    }

    std::size_t index = keyword.size();
    if (index < line.size() && !isHorizontalWhitespace(line[index])) {
        return {false, {}, "expected whitespace after '%include'"};
    }
    while (index < line.size() && isHorizontalWhitespace(line[index])) {
        ++index;
    }

    if (index >= line.size() || (line[index] != '"' && line[index] != '\'')) {
        return {false, {}, "expected a quoted path after '%include'"};
    }

    const char quote = line[index++];
    std::string path;
    bool terminated = false;
    while (index < line.size()) {
        const char c = line[index++];
        if (c == quote) {
            terminated = true;
            break;
        }
        if (c == '\\' && index < line.size()) {
            const char escaped = line[index];
            if (escaped == quote || escaped == '\\') {
                path.push_back(escaped);
                ++index;
                continue;
            }
        }
        path.push_back(c);
    }

    if (!terminated) {
        return {false, {}, "unterminated quoted path in '%include'"};
    }
    if (path.empty()) {
        return {false, {}, "include path cannot be empty"};
    }

    while (index < line.size() && isHorizontalWhitespace(line[index])) {
        ++index;
    }
    if (index < line.size() && !line.substr(index).starts_with("//")) {
        return {false, {}, "unexpected text after '%include' path"};
    }

    return {true, std::move(path), {}};
}

std::optional<std::filesystem::path> Preprocessor::resolveInclude(
    std::string_view includePath,
    const std::filesystem::path& includingFile,
    const SourceOrigin& directiveOrigin) {
    const std::filesystem::path requested {std::string(includePath)};
    if (requested.extension() != ".snh") {
        addDiagnostic(
            PreprocessSeverity::Error,
            "included files must use the '.snh' extension",
            directiveOrigin.file,
            directiveOrigin.location);
        return std::nullopt;
    }

    const std::filesystem::path baseDirectory = includingFile.parent_path();
    const std::filesystem::path candidate = requested.is_absolute()
        ? requested
        : baseDirectory / requested;
    const std::filesystem::path resolved = normalizedPath(candidate);

    std::error_code error;
    if (!std::filesystem::exists(resolved, error) || error) {
        addDiagnostic(
            PreprocessSeverity::Error,
            "included header '" + pathForMessage(resolved) + "' does not exist",
            directiveOrigin.file,
            directiveOrigin.location);
        return std::nullopt;
    }
    if (!std::filesystem::is_regular_file(resolved, error) || error) {
        addDiagnostic(
            PreprocessSeverity::Error,
            "included path '" + pathForMessage(resolved) + "' is not a regular file",
            directiveOrigin.file,
            directiveOrigin.location);
        return std::nullopt;
    }

    return resolved;
}

std::filesystem::path Preprocessor::normalizedPath(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::absolute(path, error);
    if (error) {
        absolute = path;
    }

    std::filesystem::path canonical = std::filesystem::weakly_canonical(absolute, error);
    if (!error) {
        return canonical;
    }
    return absolute.lexically_normal();
}

bool Preprocessor::isHeaderPath(const std::filesystem::path& path) {
    return path.extension() == ".snh";
}

bool Preprocessor::isInPathList(
    const std::vector<std::filesystem::path>& paths,
    const std::filesystem::path& path) {
    return std::find(paths.begin(), paths.end(), path) != paths.end();
}

void Preprocessor::updateLexicalState(
    std::string_view line,
    LexicalState& state,
    char& stringQuote) {
    std::size_t index = 0;
    while (index < line.size()) {
        const char c = line[index];
        const char next = index + 1 < line.size() ? line[index + 1] : '\0';

        if (state == LexicalState::BlockComment) {
            if (c == '*' && next == '/') {
                state = LexicalState::Normal;
                index += 2;
            } else {
                ++index;
            }
            continue;
        }

        if (state == LexicalState::String) {
            if (c == '\\') {
                // A trailing backslash continues the string on the next line.
                if (index + 1 == line.size()) {
                    return;
                }
                index += 2;
            } else if (c == stringQuote) {
                state = LexicalState::Normal;
                stringQuote = '\0';
                ++index;
            } else {
                ++index;
            }
            continue;
        }

        if (c == '/' && next == '/') {
            return;
        }
        if (c == '/' && next == '*') {
            state = LexicalState::BlockComment;
            index += 2;
            continue;
        }
        if (c == '"' || c == '\'') {
            state = LexicalState::String;
            stringQuote = c;
            ++index;
            continue;
        }
        ++index;
    }

    // Serenity strings may span physical lines only through a trailing \\.
    // Without that continuation, reset here so the lexer can diagnose the
    // unterminated literal without suppressing later preprocessor directives.
    if (state == LexicalState::String) {
        state = LexicalState::Normal;
        stringQuote = '\0';
    }
}

bool Preprocessor::hasLineContinuation(std::string_view line) {
    // A trailing backslash only joins a logical line when it is outside a
    // line comment. Ignore comment markers that occur inside quoted strings.
    bool inString = false;
    char quote = '\0';
    bool escaped = false;

    for (std::size_t index = 0; index < line.size(); ++index) {
        const char c = line[index];
        const char next = index + 1 < line.size() ? line[index + 1] : '\0';

        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == quote) {
                inString = false;
                quote = '\0';
            }
            continue;
        }

        if (c == '/' && next == '/') {
            return false;
        }
        if (c == '"' || c == '\'') {
            inString = true;
            quote = c;
        }
    }

    std::size_t backslashes = 0;
    for (std::size_t index = line.size(); index > 0 && line[index - 1] == '\\'; --index) {
        ++backslashes;
    }
    return (backslashes % 2) == 1;
}

} // namespace serenity
