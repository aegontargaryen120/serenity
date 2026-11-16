#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace serenity {

enum class TokenType {
    // Special
    Invalid,
    EndOfFile,

    // Literals and identifiers
    Identifier,
    IntegerLiteral,
    FloatingLiteral,
    StringLiteral,

    // Keywords
    KwMut,
    KwFunc,
    KwReturn,
    KwIf,
    KwElse,
    KwWhile,
    KwFor,
    KwBreak,
    KwContinue,
    KwStruct,
    KwEnum,
    KwInt,
    KwFloat,
    KwDouble,
    KwBool,
    KwString,
    KwTrue,
    KwFalse,

    // Arithmetic operators
    Plus,
    Minus,
    Star,
    Slash,
    Modulo,          // %%

    // Assignment operators
    Assign,          // =
    PlusEqual,
    MinusEqual,
    StarEqual,
    SlashEqual,
    ModuloEqual,     // %%=

    // Comparison operators
    EqualEqual,
    NotEqual,        // =/=
    Less,
    Greater,
    LessEqual,
    GreaterEqual,

    // Logical operators
    LogicalAnd,
    LogicalOr,
    LogicalNot,

    // Bitwise operators
    BitAnd,
    BitOr,
    BitXor,
    BitNot,
    ShiftLeft,
    ShiftRight,

    // Other operators and punctuation
    Arrow,           // ->
    FatArrow,        // =>
    Dot,
    Comma,
    Semicolon,
    Colon,
    LeftParen,
    RightParen,
    LeftBrace,
    RightBrace,
    LeftBracket,
    RightBracket,
};

struct SourceLocation {
    std::size_t offset {0};
    std::size_t line {1};
    std::size_t column {1};
};

struct Token {
    TokenType type {TokenType::Invalid};
    std::string lexeme;
    SourceLocation location;

    // Populated only for string literals. The lexeme retains the original source
    // spelling; stringValue contains the decoded UTF-8 byte sequence.
    std::optional<std::string> stringValue;
};

[[nodiscard]] std::string_view tokenTypeName(TokenType type) noexcept;

} // namespace serenity
