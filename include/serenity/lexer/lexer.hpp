#pragma once

#include "serenity/lexer/token.hpp"

#include <string>
#include <vector>

namespace serenity {

struct LexError {
    std::string message;
    SourceLocation location;
};

struct LexResult {
    std::vector<Token> tokens;
    std::vector<LexError> errors;

    [[nodiscard]] bool ok() const noexcept { return errors.empty(); }
};

class Lexer {
public:
    // The lexer owns its source so callers may safely pass temporary strings.
    explicit Lexer(std::string source);

    [[nodiscard]] LexResult tokenize();

private:
    std::string source_;
    std::size_t current_ {0};
    std::size_t line_ {1};
    std::size_t column_ {1};
    bool previousWasCR_ {false};

    std::size_t tokenStart_ {0};
    SourceLocation tokenLocation_ {};

    std::vector<Token> tokens_;
    std::vector<LexError> errors_;

    // A backslash-newline outside a string joins the following source line.
    // If the preceding and following tokens are strings, their values are
    // concatenated, as in the Serenity string-continuation syntax.
    bool pendingStringConcat_ {false};

    [[nodiscard]] bool atEnd() const noexcept;
    [[nodiscard]] char peek(std::size_t lookahead = 0) const noexcept;
    char advance() noexcept;
    bool match(char expected) noexcept;

    void skipWhitespaceAndComments();
    void scanToken();
    void scanIdentifier();
    void scanNumber();
    void scanString(char quote);

    void addToken(TokenType type);
    void addStringToken(std::string decodedValue);
    void addError(std::string message, SourceLocation location);

    [[nodiscard]] bool isAlpha(char c) const noexcept;
    [[nodiscard]] bool isDigit(char c) const noexcept;
    [[nodiscard]] bool isAlphaNumeric(char c) const noexcept;
};

} // namespace serenity
