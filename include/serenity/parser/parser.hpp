#pragma once

#include "serenity/ast/ast.hpp"
#include "serenity/lexer/token.hpp"

#include <memory>
#include <string>
#include <vector>

namespace serenity::parser {

struct ParseError {
    std::string message;
    SourceLocation location {};
};

struct ParseResult {
    std::unique_ptr<ast::Program> program;
    std::vector<ParseError> errors;

    [[nodiscard]] bool ok() const noexcept {
        return program != nullptr && errors.empty();
    }
};

// Recursive-descent parser with precedence-climbing expression parsing.
// The parser owns its token stream. Pass the tokens produced by Lexer::tokenize().
class Parser {
public:
    explicit Parser(std::vector<Token> tokens);

    [[nodiscard]] ParseResult parse();

private:
    struct ParseFailure {};

    std::vector<Token> tokens_;
    std::size_t current_ {0};
    std::vector<ParseError> errors_;

    [[nodiscard]] const Token& peek() const noexcept;
    [[nodiscard]] const Token& previous() const noexcept;
    [[nodiscard]] const Token& lookahead(std::size_t distance) const noexcept;
    [[nodiscard]] bool atEnd() const noexcept;
    [[nodiscard]] bool check(TokenType type) const noexcept;
    [[nodiscard]] bool match(TokenType type) noexcept;
    const Token& advance() noexcept;
    const Token& consume(TokenType type, const std::string& message);

    void report(std::string message, SourceLocation location);
    void synchronizeTopLevel() noexcept;
    void synchronizeStatement() noexcept;

    [[nodiscard]] ast::SourceSpan spanFrom(SourceLocation begin) const noexcept;
    [[nodiscard]] ast::SourceSpan spanFromPrevious() const noexcept;

    [[nodiscard]] ast::DeclPtr parseTopLevelDeclaration();
    [[nodiscard]] ast::DeclPtr parseGlobalVariableDeclaration();
    [[nodiscard]] ast::DeclPtr parseFunctionDeclarationOrDefinition();
    [[nodiscard]] ast::DeclPtr parseSignatureImplementation();
    [[nodiscard]] ast::DeclPtr parseStructDeclaration();
    [[nodiscard]] ast::DeclPtr parseEnumDeclaration();

    [[nodiscard]] ast::VariableSpec parseVariableSpec(
        bool allowMutability,
        bool consumeSemicolon = true);
    [[nodiscard]] ast::TypeRef parseType();
    [[nodiscard]] ast::Parameter parseParameter(bool allowMissingName);
    [[nodiscard]] std::vector<ast::Parameter> parseFunctionParameters(
        bool allowMissingNames,
        bool signatureOnly);

    [[nodiscard]] ast::StmtPtr parseStatement();
    [[nodiscard]] std::unique_ptr<ast::BlockStmt> parseBlock();
    [[nodiscard]] ast::StmtPtr parseVariableDeclarationStatement(bool consumeSemicolon = true);
    [[nodiscard]] ast::StmtPtr parseExpressionOrAssignmentStatement(bool consumeSemicolon = true);
    [[nodiscard]] ast::StmtPtr parseReturnStatement();
    [[nodiscard]] ast::StmtPtr parseIfStatement();
    [[nodiscard]] ast::StmtPtr parseWhileStatement();
    [[nodiscard]] ast::StmtPtr parseForStatement();

    [[nodiscard]] ast::ExprPtr parseInitializer();
    [[nodiscard]] ast::ExprPtr parseStructInitializer();
    [[nodiscard]] ast::ExprPtr parseExpression();
    [[nodiscard]] ast::ExprPtr parseBinaryExpression(int minimumPrecedence);
    [[nodiscard]] ast::ExprPtr parseUnaryExpression();
    [[nodiscard]] ast::ExprPtr parsePostfixExpression();
    [[nodiscard]] ast::ExprPtr parsePrimaryExpression();
    [[nodiscard]] ast::ExprPtr parseFunctionCall(const Token& nameToken);
    [[nodiscard]] std::vector<ast::ExprPtr> parseArguments();

    [[nodiscard]] bool isTypeStart(const Token& token) const noexcept;
    [[nodiscard]] bool looksLikeLocalVariableDeclaration() const noexcept;
    [[nodiscard]] bool looksLikeSignatureImplementation() const noexcept;
    [[nodiscard]] bool isAssignmentOperator(TokenType type) const noexcept;
    [[nodiscard]] int precedence(TokenType type) const noexcept;
    [[nodiscard]] ast::BinaryOperator binaryOperator(TokenType type) const;
    [[nodiscard]] ast::AssignmentOperator assignmentOperator(TokenType type) const;
    [[nodiscard]] ast::UnaryOperator unaryOperator(TokenType type) const;
    [[nodiscard]] bool isAssignable(const ast::Expression& expression) const noexcept;
};

} // namespace serenity::parser
