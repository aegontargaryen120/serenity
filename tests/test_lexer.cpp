#include "serenity/lexer/lexer.hpp"

#include <cstdlib>
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

} // namespace

int main() {
    const std::string source = R"SN(
// Variable declarations and both string quote styles.
mut int age = 17;
string name = "Aegon";
string surname = 'Targaryen';
float pi = 3.14159;
double precise = 2.0;
bool alive = true;

/* A block comment. */
string first = "Hello, \
world!";
string second = "Hello,\n" \
"world!";

if (age >= 18 && alive) {
    println(name);
} else {
    println(surname);
}

int remainder = 17 %% 5;
int changed = 17;
changed %%= 2;
bool unequal = age =/= 18;
func add(int, int) => int;
)SN";

    serenity::Lexer lexer(source);
    const serenity::LexResult result = lexer.tokenize();

    if (!result.ok()) {
        for (const auto& error : result.errors) {
            std::cerr << error.location.line << ':' << error.location.column
                      << ": " << error.message << '\n';
        }
    }
    require(result.ok(), "valid Serenity sample should tokenize without errors");

    auto findString = [&result](std::string_view expected) {
        for (const auto& token : result.tokens) {
            if (token.type == serenity::TokenType::StringLiteral && token.stringValue
                && *token.stringValue == expected) {
                return true;
            }
        }
        return false;
    };

    require(findString("Aegon"), "double-quoted string should decode");
    require(findString("Targaryen"), "single-quoted string should decode");
    require(findString("Hello, world!"), "backslash-newline inside a string should be removed");
    require(findString("Hello,\nworld!"), "continued adjacent string literals should concatenate");

    auto contains = [&result](serenity::TokenType type) {
        for (const auto& token : result.tokens) {
            if (token.type == type) return true;
        }
        return false;
    };

    require(contains(serenity::TokenType::KwMut), "mut keyword should be recognized");
    require(contains(serenity::TokenType::KwFunc), "func keyword should be recognized");
    require(contains(serenity::TokenType::FloatingLiteral), "decimal floating literal should be recognized");
    require(contains(serenity::TokenType::Modulo), "%% should be the remainder operator");
    require(contains(serenity::TokenType::ModuloEqual), "%%= should be the remainder assignment operator");
    require(contains(serenity::TokenType::NotEqual), "=/= should be the not-equal operator");
    require(contains(serenity::TokenType::FatArrow), "=> should be recognized");

    // Verify that the old spellings are diagnosed rather than accepted.
    serenity::Lexer invalidLexer("int x = 10 % 3; bool b = x != 3;");
    const serenity::LexResult invalidResult = invalidLexer.tokenize();
    require(!invalidResult.ok(), "bare % and != should produce lexer diagnostics");

    std::cout << "All Serenity lexer tests passed (" << result.tokens.size() << " tokens).\n";
    return EXIT_SUCCESS;
}
