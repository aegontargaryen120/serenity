#include "serenity/preprocessor/preprocessor.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void writeFile(const std::filesystem::path& path, std::string_view contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    require(static_cast<bool>(output), "test fixture should open for writing");
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    require(static_cast<bool>(output), "test fixture should write successfully");
}

std::size_t countOccurrences(std::string_view text, std::string_view needle) {
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = text.find(needle, position)) != std::string_view::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

} // namespace

int main() {
    const std::filesystem::path root = std::filesystem::temp_directory_path()
        / "serenity_preprocessor_v01_tests";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);

    writeFile(root / "main.sn",
        "%include \"include/a.snh\"\n"
        "%include \"include/a.snh\" // duplicate, automatically guarded\n"
        "string after = \"main source\";\n");
    writeFile(root / "include/a.snh",
        "func alpha(int) => int;\n"
        "%include \"nested/b.snh\"\n");
    writeFile(root / "include/nested/b.snh",
        "func beta(int) => int;\n");

    serenity::Preprocessor preprocessor;
    const serenity::PreprocessResult result = preprocessor.preprocessFile(root / "main.sn");
    if (!result.ok()) {
        for (const auto& diagnostic : result.diagnostics) {
            std::cerr << diagnostic.origin.file << ':' << diagnostic.origin.location.line
                      << ':' << diagnostic.origin.location.column << ": "
                      << diagnostic.message << '\n';
        }
    }
    require(result.ok(), "valid nested includes should preprocess successfully");
    require(countOccurrences(result.source, "func alpha") == 1,
        "the same header should be included only once automatically");
    require(countOccurrences(result.source, "func beta") == 1,
        "nested relative includes should expand");
    require(result.source.find("string after = \"main source\";") != std::string::npos,
        "source following an include should remain present");

    const std::size_t betaOffset = result.source.find("func beta");
    require(betaOffset != std::string::npos, "expanded nested header text should exist");
    const auto betaOrigin = result.originAt(betaOffset);
    require(betaOrigin.has_value(), "source map should resolve an expanded offset");
    require(betaOrigin->file.filename() == "b.snh",
        "source map should identify the original nested header");
    require(betaOrigin->location.line == 1,
        "source map should preserve the original header line number");

    // Directive-looking text in comments and strings must not be expanded.
    const std::string commentAndStringSource =
        "/*\n"
        "%include \"does-not-exist.snh\"\n"
        "*/\n"
        "string text = \"%include 'also-not-real.snh'\";\n";
    const serenity::PreprocessResult ignoredDirectives =
        preprocessor.preprocessSource(commentAndStringSource, root / "comments.sn");
    require(ignoredDirectives.ok(), "directive-looking text in comments and strings should be ignored");
    require(ignoredDirectives.source.find("does-not-exist.snh") != std::string::npos,
        "comment contents should be preserved");

    // Automatic guards also terminate include cycles. The cycle is reported as
    // a warning, not an error, because expansion can continue safely.
    writeFile(root / "cycle_a.snh",
        "%include \"cycle_b.snh\"\n"
        "func cycleA() => int { return 1; }\n");
    writeFile(root / "cycle_b.snh",
        "%include \"cycle_a.snh\"\n"
        "func cycleB() => int { return 2; }\n");
    writeFile(root / "cycle_main.sn", "%include \"cycle_a.snh\"\n");
    const serenity::PreprocessResult cycleResult = preprocessor.preprocessFile(root / "cycle_main.sn");
    require(cycleResult.ok(), "a guarded include cycle should not be fatal");
    require(countOccurrences(cycleResult.source, "func cycleA") == 1,
        "the first cycle header should be expanded once");
    require(countOccurrences(cycleResult.source, "func cycleB") == 1,
        "the second cycle header should be expanded once");
    require(!cycleResult.diagnostics.empty()
            && cycleResult.diagnostics.front().severity == serenity::PreprocessSeverity::Warning,
        "a circular include should produce a warning");

    const serenity::PreprocessResult missing = preprocessor.preprocessSource(
        "%include \"missing.snh\"\n", root / "missing_main.sn");
    require(!missing.ok(), "missing headers should produce an error");
    require(!missing.diagnostics.empty() && missing.diagnostics.front().origin.location.line == 1,
        "include diagnostics should point to the directive line");

    const serenity::PreprocessResult wrongExtension = preprocessor.preprocessSource(
        "%include \"other.sn\"\n", root / "bad_extension.sn");
    require(!wrongExtension.ok(), "only .snh files should be includable");

    const serenity::PreprocessResult malformed = preprocessor.preprocessSource(
        "%include missing_quotes.snh\n", root / "malformed.sn");
    require(!malformed.ok(), "include paths must be quoted");

    const serenity::PreprocessResult continuation = preprocessor.preprocessSource(
        "int value = 1 + \\\n"
        "%include \"missing-on-continued-line.snh\";\n",
        root / "continued.sn");
    require(continuation.ok(),
        "a percent sign on a backslash-continued logical line must not start a directive");

    std::filesystem::remove_all(root, ignored);
    std::cout << "All Serenity preprocessor tests passed.\n";
    return EXIT_SUCCESS;
}
