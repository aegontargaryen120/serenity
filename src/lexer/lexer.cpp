#include "serenity/lexer/lexer.hpp"

#include <unordered_map>
#include <utility>

namespace serenity {
namespace {

[[nodiscard]] const std::unordered_map<std::string_view, TokenType>& keywords() {
    static const std::unordered_map<std::string_view, TokenType> table {
        {"mut", TokenType::KwMut},
        {"func", TokenType::KwFunc},
        {"return", TokenType::KwReturn},
        {"if", TokenType::KwIf},
        {"else", TokenType::KwElse},
        {"while", TokenType::KwWhile},
        {"for", TokenType::KwFor},
        {"break", TokenType::KwBreak},
        {"continue", TokenType::KwContinue},
        {"struct", TokenType::KwStruct},
        {"enum", TokenType::KwEnum},
        {"int", TokenType::KwInt},
        {"float", TokenType::KwFloat},
        {"double", TokenType::KwDouble},
        {"bool", TokenType::KwBool},
        {"string", TokenType::KwString},
        {"any", TokenType::KwAny},
        {"stream", TokenType::KwStream},
        {"void", TokenType::KwVoid},
        {"true", TokenType::KwTrue},
        {"false", TokenType::KwFalse},
        {"null", TokenType::KwNull},
    };
    return table;
}

} // namespace

std::string_view tokenTypeName(TokenType type) noexcept {
    switch (type) {
    case TokenType::Invalid: return "INVALID";
    case TokenType::EndOfFile: return "EOF";
    case TokenType::Identifier: return "IDENTIFIER";
    case TokenType::IntegerLiteral: return "INTEGER_LITERAL";
    case TokenType::FloatingLiteral: return "FLOATING_LITERAL";
    case TokenType::StringLiteral: return "STRING_LITERAL";
    case TokenType::KwMut: return "mut";
    case TokenType::KwFunc: return "func";
    case TokenType::KwReturn: return "return";
    case TokenType::KwIf: return "if";
    case TokenType::KwElse: return "else";
    case TokenType::KwWhile: return "while";
    case TokenType::KwFor: return "for";
    case TokenType::KwBreak: return "break";
    case TokenType::KwContinue: return "continue";
    case TokenType::KwStruct: return "struct";
    case TokenType::KwEnum: return "enum";
    case TokenType::KwInt: return "int";
    case TokenType::KwFloat: return "float";
    case TokenType::KwDouble: return "double";
    case TokenType::KwBool: return "bool";
    case TokenType::KwString: return "string";
    case TokenType::KwAny: return "any";
    case TokenType::KwStream: return "stream";
    case TokenType::KwVoid: return "void";
    case TokenType::KwTrue: return "true";
    case TokenType::KwFalse: return "false";
    case TokenType::KwNull: return "null";
    case TokenType::Plus: return "+";
    case TokenType::Minus: return "-";
    case TokenType::Star: return "*";
    case TokenType::Slash: return "/";
    case TokenType::Modulo: return "%%";
    case TokenType::Assign: return "=";
    case TokenType::PlusEqual: return "+=";
    case TokenType::MinusEqual: return "-=";
    case TokenType::StarEqual: return "*=";
    case TokenType::SlashEqual: return "/=";
    case TokenType::ModuloEqual: return "%%=";
    case TokenType::EqualEqual: return "==";
    case TokenType::NotEqual: return "=/=";
    case TokenType::Less: return "<";
    case TokenType::Greater: return ">";
    case TokenType::LessEqual: return "<=";
    case TokenType::GreaterEqual: return ">=";
    case TokenType::LogicalAnd: return "&&";
    case TokenType::LogicalOr: return "||";
    case TokenType::LogicalNot: return "!";
    case TokenType::BitAnd: return "&";
    case TokenType::BitOr: return "|";
    case TokenType::BitXor: return "^";
    case TokenType::BitNot: return "~";
    case TokenType::ShiftLeft: return "<<";
    case TokenType::ShiftRight: return ">>";
    case TokenType::Arrow: return "->";
    case TokenType::FatArrow: return "=>";
    case TokenType::Dot: return ".";
    case TokenType::Comma: return ",";
    case TokenType::Semicolon: return ";";
    case TokenType::Colon: return ":";
    case TokenType::LeftParen: return "(";
    case TokenType::RightParen: return ")";
    case TokenType::LeftBrace: return "{";
    case TokenType::RightBrace: return "}";
    case TokenType::LeftBracket: return "[";
    case TokenType::RightBracket: return "]";
    }
    return "UNKNOWN_TOKEN";
}

Lexer::Lexer(std::string source)
    : source_(std::move(source))
{
}

LexResult Lexer::tokenize() {
    tokens_.clear();
    errors_.clear();
    current_ = 0;
    line_ = 1;
    column_ = 1;
    previousWasCR_ = false;
    pendingStringConcat_ = false;

    while (!atEnd()) {
        skipWhitespaceAndComments();
        if (!atEnd()) {
            scanToken();
        }
    }

    tokens_.push_back(Token {
        .type = TokenType::EndOfFile,
        .lexeme = "",
        .location = SourceLocation {current_, line_, column_},
        .stringValue = std::nullopt,
    });

    return LexResult {std::move(tokens_), std::move(errors_)};
}

bool Lexer::atEnd() const noexcept {
    return current_ >= source_.size();
}

char Lexer::peek(std::size_t lookahead) const noexcept {
    const std::size_t index = current_ + lookahead;
    return index < source_.size() ? source_[index] : '\0';
}

char Lexer::advance() noexcept {
    if (atEnd()) {
        return '\0';
    }

    const char c = source_[current_++];
    if (c == '\r') {
        ++line_;
        column_ = 1;
        previousWasCR_ = true;
    } else if (c == '\n') {
        if (!previousWasCR_) {
            ++line_;
        }
        column_ = 1;
        previousWasCR_ = false;
    } else {
        ++column_;
        previousWasCR_ = false;
    }
    return c;
}

bool Lexer::match(char expected) noexcept {
    if (atEnd() || peek() != expected) {
        return false;
    }
    (void)advance();
    return true;
}

void Lexer::skipWhitespaceAndComments() {
    for (;;) {
        const char c = peek();

        if (c == ' ' || c == '\t' || c == '\f' || c == '\v' || c == '\r' || c == '\n') {
            (void)advance();
            continue;
        }

        // Backslash-newline joins physical source lines. It is also the
        // separator used by Serenity's continued adjacent string literals.
        if (c == '\\' && (peek(1) == '\n' || peek(1) == '\r')) {
            (void)advance();
            (void)advance();
            if (peek() == '\n' && previousWasCR_) {
                (void)advance();
            }
            if (!tokens_.empty() && tokens_.back().type == TokenType::StringLiteral) {
                pendingStringConcat_ = true;
            }
            continue;
        }

        if (c == '/' && peek(1) == '/') {
            (void)advance();
            (void)advance();
            while (!atEnd() && peek() != '\n' && peek() != '\r') {
                (void)advance();
            }
            continue;
        }

        if (c == '/' && peek(1) == '*') {
            const SourceLocation commentStart {current_, line_, column_};
            (void)advance();
            (void)advance();
            bool terminated = false;
            while (!atEnd()) {
                if (peek() == '*' && peek(1) == '/') {
                    (void)advance();
                    (void)advance();
                    terminated = true;
                    break;
                }
                (void)advance();
            }
            if (!terminated) {
                addError("unterminated block comment", commentStart);
            }
            continue;
        }

        break;
    }
}

void Lexer::scanToken() {
    tokenStart_ = current_;
    tokenLocation_ = SourceLocation {current_, line_, column_};

    const char c = advance();

    // A line continuation only concatenates strings when the next significant
    // token is another string literal. Any other token cancels that pending
    // concatenation, including identifiers and numeric literals.
    if (pendingStringConcat_ && c != '"' && c != '\'') {
        pendingStringConcat_ = false;
    }

    if (isAlpha(c) || c == '_') {
        scanIdentifier();
        return;
    }

    if (isDigit(c)) {
        scanNumber();
        return;
    }

    if (c == '"' || c == '\'') {
        scanString(c);
        return;
    }

    switch (c) {
    case '+': addToken(match('=') ? TokenType::PlusEqual : TokenType::Plus); break;
    case '-':
        if (match('>')) addToken(TokenType::Arrow);
        else if (match('=')) addToken(TokenType::MinusEqual);
        else addToken(TokenType::Minus);
        break;
    case '*': addToken(match('=') ? TokenType::StarEqual : TokenType::Star); break;
    case '/': addToken(match('=') ? TokenType::SlashEqual : TokenType::Slash); break;
    case '=':
        if (match('/')) {
            if (match('=')) addToken(TokenType::NotEqual);
            else {
                addError("expected '=' after '=/'; the not-equal operator is '=/='", tokenLocation_);
                addToken(TokenType::Invalid);
            }
        } else if (match('=')) {
            addToken(TokenType::EqualEqual);
        } else if (match('>')) {
            addToken(TokenType::FatArrow);
        } else {
            addToken(TokenType::Assign);
        }
        break;
    case '!':
        if (peek() == '=') {
            (void)advance();
            addError("'!=' is not a Serenity operator; use '=/='", tokenLocation_);
            addToken(TokenType::Invalid);
        } else {
            addToken(TokenType::LogicalNot);
        }
        break;
    case '<':
        if (match('<')) addToken(TokenType::ShiftLeft);
        else if (match('=')) addToken(TokenType::LessEqual);
        else addToken(TokenType::Less);
        break;
    case '>':
        if (match('>')) addToken(TokenType::ShiftRight);
        else if (match('=')) addToken(TokenType::GreaterEqual);
        else addToken(TokenType::Greater);
        break;
    case '&': addToken(match('&') ? TokenType::LogicalAnd : TokenType::BitAnd); break;
    case '|': addToken(match('|') ? TokenType::LogicalOr : TokenType::BitOr); break;
    case '^': addToken(TokenType::BitXor); break;
    case '~': addToken(TokenType::BitNot); break;
    case '%':
        if (match('%')) {
            addToken(match('=') ? TokenType::ModuloEqual : TokenType::Modulo);
        } else {
            addError("'%' is reserved for preprocessor directives; use '%%' for remainder", tokenLocation_);
            addToken(TokenType::Invalid);
        }
        break;
    case '.': addToken(TokenType::Dot); break;
    case ',': addToken(TokenType::Comma); break;
    case ';': addToken(TokenType::Semicolon); break;
    case ':': addToken(TokenType::Colon); break;
    case '(': addToken(TokenType::LeftParen); break;
    case ')': addToken(TokenType::RightParen); break;
    case '{': addToken(TokenType::LeftBrace); break;
    case '}': addToken(TokenType::RightBrace); break;
    case '[': addToken(TokenType::LeftBracket); break;
    case ']': addToken(TokenType::RightBracket); break;
    default:
        addError("unexpected character in source", tokenLocation_);
        addToken(TokenType::Invalid);
        break;
    }
}

void Lexer::scanIdentifier() {
    while (isAlphaNumeric(peek()) || peek() == '_') {
        (void)advance();
    }

    const std::string_view text(source_.data() + tokenStart_, current_ - tokenStart_);
    const auto it = keywords().find(text);
    addToken(it == keywords().end() ? TokenType::Identifier : it->second);
}

void Lexer::scanNumber() {
    while (isDigit(peek())) {
        (void)advance();
    }

    bool isFloating = false;
    if (peek() == '.' && isDigit(peek(1))) {
        isFloating = true;
        (void)advance();
        while (isDigit(peek())) {
            (void)advance();
        }
    }

    // Numeric bases and exponent notation are deliberately reserved for a
    // later Serenity version. Reject them as one malformed literal instead
    // of silently splitting, for example, 0xFF into two unrelated tokens.
    if (isAlpha(peek()) || peek() == '_') {
        while (isAlphaNumeric(peek()) || peek() == '_') {
            (void)advance();
        }
        addError("unsupported or malformed numeric literal; v0.1 supports decimal integers and decimal floating-point literals only", tokenLocation_);
        addToken(TokenType::Invalid);
        return;
    }

    addToken(isFloating ? TokenType::FloatingLiteral : TokenType::IntegerLiteral);
}

void Lexer::scanString(char quote) {
    std::string decoded;
    bool terminated = false;

    while (!atEnd()) {
        const char c = advance();

        if (c == quote) {
            terminated = true;
            break;
        }

        if (c == '\n' || c == '\r') {
            addError("unescaped newline in string literal; use a backslash line continuation or an escaped newline", tokenLocation_);
            break;
        }

        if (c != '\\') {
            decoded.push_back(c);
            continue;
        }

        if (atEnd()) {
            addError("incomplete escape sequence at end of file", tokenLocation_);
            break;
        }

        const char escaped = advance();
        if (escaped == '\n' || escaped == '\r') {
            // Backslash-newline inside a string is removed entirely.
            if (escaped == '\r' && peek() == '\n') {
                (void)advance();
            }
            continue;
        }

        switch (escaped) {
        case 'a': decoded.push_back('\a'); break;
        case 'b': decoded.push_back('\b'); break;
        case 'f': decoded.push_back('\f'); break;
        case 'n': decoded.push_back('\n'); break;
        case 'r': decoded.push_back('\r'); break;
        case 't': decoded.push_back('\t'); break;
        case 'v': decoded.push_back('\v'); break;
        case '\\': decoded.push_back('\\'); break;
        case '\'': decoded.push_back('\''); break;
        case '"': decoded.push_back('"'); break;
        case '0': decoded.push_back('\0'); break;
        default:
            addError("unknown escape sequence in string literal", SourceLocation {current_ - 2, line_, column_ >= 2 ? column_ - 2 : 1});
            decoded.push_back(escaped);
            break;
        }
    }

    if (!terminated && atEnd()) {
        addError("unterminated string literal", tokenLocation_);
    }

    addStringToken(std::move(decoded));
}

void Lexer::addToken(TokenType type) {
    tokens_.push_back(Token {
        .type = type,
        .lexeme = source_.substr(tokenStart_, current_ - tokenStart_),
        .location = tokenLocation_,
        .stringValue = std::nullopt,
    });
}

void Lexer::addStringToken(std::string decodedValue) {
    std::string lexeme = source_.substr(tokenStart_, current_ - tokenStart_);

    if (pendingStringConcat_ && !tokens_.empty() && tokens_.back().type == TokenType::StringLiteral) {
        if (!tokens_.back().stringValue) {
            tokens_.back().stringValue.emplace();
        }
        tokens_.back().stringValue->append(decodedValue);
        tokens_.back().lexeme.append(" ");
        tokens_.back().lexeme.append(lexeme);
    } else {
        tokens_.push_back(Token {
            .type = TokenType::StringLiteral,
            .lexeme = std::move(lexeme),
            .location = tokenLocation_,
            .stringValue = std::move(decodedValue),
        });
    }

    pendingStringConcat_ = false;
}

void Lexer::addError(std::string message, SourceLocation location) {
    errors_.push_back(LexError {std::move(message), location});
}

bool Lexer::isAlpha(char c) const noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool Lexer::isDigit(char c) const noexcept {
    return c >= '0' && c <= '9';
}

bool Lexer::isAlphaNumeric(char c) const noexcept {
    return isAlpha(c) || isDigit(c);
}

} // namespace serenity
