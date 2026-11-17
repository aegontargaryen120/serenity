#include "serenity/parser/parser.hpp"

#include <algorithm>
#include <utility>

namespace serenity::parser {

Parser::Parser(std::vector<Token> tokens)
    : tokens_(std::move(tokens))
{
    if (tokens_.empty() || tokens_.back().type != TokenType::EndOfFile) {
        SourceLocation location {};
        if (!tokens_.empty()) {
            const Token& last = tokens_.back();
            location = last.location;
            location.offset += last.lexeme.size();
            location.column += last.lexeme.size();
        }
        tokens_.push_back(Token {
            .type = TokenType::EndOfFile,
            .lexeme = {},
            .location = location,
            .stringValue = std::nullopt,
        });
    }
}

ParseResult Parser::parse() {
    current_ = 0;
    errors_.clear();

    const SourceLocation programStart = peek().location;
    std::vector<ast::DeclPtr> declarations;

    while (!atEnd()) {
        const std::size_t before = current_;
        try {
            declarations.push_back(parseTopLevelDeclaration());
        } catch (const ParseFailure&) {
            synchronizeTopLevel();
        }

        if (current_ == before && !atEnd()) {
            (void)advance();
        }
    }

    const ast::SourceSpan programSpan {
        .begin = programStart,
        .end = peek().location,
    };
    auto program = std::make_unique<ast::Program>(std::move(declarations), programSpan);
    return ParseResult {std::move(program), std::move(errors_)};
}

const Token& Parser::peek() const noexcept {
    return tokens_[std::min(current_, tokens_.size() - 1)];
}

const Token& Parser::previous() const noexcept {
    const std::size_t index = current_ == 0 ? 0 : current_ - 1;
    return tokens_[std::min(index, tokens_.size() - 1)];
}

const Token& Parser::lookahead(std::size_t distance) const noexcept {
    return tokens_[std::min(current_ + distance, tokens_.size() - 1)];
}

bool Parser::atEnd() const noexcept {
    return check(TokenType::EndOfFile);
}

bool Parser::check(TokenType type) const noexcept {
    return peek().type == type;
}

bool Parser::match(TokenType type) noexcept {
    if (!check(type)) {
        return false;
    }
    (void)advance();
    return true;
}

const Token& Parser::advance() noexcept {
    if (!atEnd()) {
        ++current_;
    }
    return previous();
}

const Token& Parser::consume(TokenType type, const std::string& message) {
    if (check(type)) {
        return advance();
    }
    report(message, peek().location);
    throw ParseFailure {};
}

void Parser::report(std::string message, SourceLocation location) {
    errors_.push_back(ParseError {std::move(message), location});
}

void Parser::synchronizeTopLevel() noexcept {
    while (!atEnd()) {
        if (current_ > 0 && previous().type == TokenType::Semicolon) {
            return;
        }

        if (check(TokenType::KwFunc) || check(TokenType::KwStruct) || check(TokenType::KwEnum)) {
            return;
        }

        if (check(TokenType::KwMut) || isTypeStart(peek())) {
            // An identifier followed by '(' can begin a signature implementation;
            // an identifier followed by another identifier can begin a global.
            if (peek().type != TokenType::Identifier ||
                lookahead(1).type == TokenType::Identifier ||
                lookahead(1).type == TokenType::LeftParen) {
                return;
            }
        }
        (void)advance();
    }
}

void Parser::synchronizeStatement() noexcept {
    while (!atEnd() && !check(TokenType::RightBrace)) {
        if (current_ > 0 && previous().type == TokenType::Semicolon) {
            return;
        }

        switch (peek().type) {
        case TokenType::KwMut:
        case TokenType::KwReturn:
        case TokenType::KwIf:
        case TokenType::KwWhile:
        case TokenType::KwFor:
        case TokenType::KwBreak:
        case TokenType::KwContinue:
        case TokenType::LeftBrace:
            return;
        default:
            break;
        }
        (void)advance();
    }
}

ast::SourceSpan Parser::spanFrom(SourceLocation begin) const noexcept {
    return ast::SourceSpan {.begin = begin, .end = peek().location};
}

ast::SourceSpan Parser::spanFromPrevious() const noexcept {
    return spanFrom(previous().location);
}

ast::DeclPtr Parser::parseTopLevelDeclaration() {
    if (check(TokenType::KwFunc)) {
        return parseFunctionDeclarationOrDefinition();
    }
    if (check(TokenType::KwStruct)) {
        return parseStructDeclaration();
    }
    if (check(TokenType::KwEnum)) {
        return parseEnumDeclaration();
    }
    if (looksLikeSignatureImplementation()) {
        return parseSignatureImplementation();
    }
    if (check(TokenType::KwMut) || isTypeStart(peek())) {
        return parseGlobalVariableDeclaration();
    }

    report("expected a top-level declaration; executable statements must be inside a function", peek().location);
    throw ParseFailure {};
}

ast::DeclPtr Parser::parseGlobalVariableDeclaration() {
    const SourceLocation begin = peek().location;
    ast::VariableSpec variable = parseVariableSpec(true);
    const ast::SourceSpan span = spanFrom(begin);
    variable.span = span;
    return std::make_unique<ast::GlobalVariableDecl>(std::move(variable), span);
}

ast::DeclPtr Parser::parseFunctionDeclarationOrDefinition() {
    const SourceLocation begin = consume(TokenType::KwFunc, "expected 'func'").location;
    const Token& name = consume(TokenType::Identifier, "expected function name after 'func'");
    (void)consume(TokenType::LeftParen, "expected '(' after function name");

    std::vector<ast::Parameter> parameters = parseFunctionParameters(true, false);
    (void)consume(TokenType::RightParen, "expected ')' after function parameters");
    (void)consume(TokenType::FatArrow, "expected '=>' before function return type");
    ast::TypeRef returnType = parseType();

    if (match(TokenType::Semicolon)) {
        const ast::SourceSpan span = spanFrom(begin);
        return std::make_unique<ast::FunctionDecl>(
            name.lexeme,
            std::move(parameters),
            std::move(returnType),
            ast::FunctionForm::Prototype,
            nullptr,
            span);
    }

    for (const ast::Parameter& parameter : parameters) {
        if (!parameter.name.has_value()) {
            report("function definitions require a name for every parameter", peek().location);
            throw ParseFailure {};
        }
    }

    auto body = parseBlock();
    const ast::SourceSpan span = spanFrom(begin);
    return std::make_unique<ast::FunctionDecl>(
        name.lexeme,
        std::move(parameters),
        std::move(returnType),
        ast::FunctionForm::TypedDefinition,
        std::move(body),
        span);
}

ast::DeclPtr Parser::parseSignatureImplementation() {
    const Token& name = consume(TokenType::Identifier, "expected function name");
    const SourceLocation begin = name.location;
    (void)consume(TokenType::LeftParen, "expected '(' after function name");

    std::vector<ast::Parameter> parameters = parseFunctionParameters(false, true);
    (void)consume(TokenType::RightParen, "expected ')' after parameter names");
    (void)consume(TokenType::FatArrow, "expected '=>' before function return type");
    ast::TypeRef returnType = parseType();
    auto body = parseBlock();

    const ast::SourceSpan span = spanFrom(begin);
    return std::make_unique<ast::FunctionDecl>(
        name.lexeme,
        std::move(parameters),
        std::move(returnType),
        ast::FunctionForm::SignatureImplementation,
        std::move(body),
        span);
}

ast::DeclPtr Parser::parseStructDeclaration() {
    const SourceLocation begin = consume(TokenType::KwStruct, "expected 'struct'").location;
    (void)consume(TokenType::LeftBrace, "expected '{' after 'struct'");

    std::vector<ast::FieldDecl> fields;
    while (!check(TokenType::RightBrace) && !atEnd()) {
        const std::size_t before = current_;
        try {
            const SourceLocation fieldBegin = peek().location;
            ast::TypeRef type = parseType();
            const Token& name = consume(TokenType::Identifier, "expected field name after field type");
            ast::ExprPtr defaultValue;
            if (match(TokenType::Assign)) {
                defaultValue = parseInitializer();
            }
            (void)consume(TokenType::Semicolon, "expected ';' after struct field");
            fields.push_back(ast::FieldDecl {
                .type = std::move(type),
                .name = name.lexeme,
                .defaultValue = std::move(defaultValue),
                .span = spanFrom(fieldBegin),
            });
        } catch (const ParseFailure&) {
            synchronizeStatement();
        }
        if (current_ == before && !check(TokenType::RightBrace) && !atEnd()) {
            (void)advance();
        }
    }

    (void)consume(TokenType::RightBrace, "expected '}' after struct fields");
    const Token& name = consume(TokenType::Identifier, "expected struct type name after '}'");
    (void)consume(TokenType::Semicolon, "expected ';' after struct declaration");

    const ast::SourceSpan span = spanFrom(begin);
    return std::make_unique<ast::StructDecl>(name.lexeme, std::move(fields), span);
}

ast::DeclPtr Parser::parseEnumDeclaration() {
    const SourceLocation begin = consume(TokenType::KwEnum, "expected 'enum'").location;
    (void)consume(TokenType::LeftBrace, "expected '{' after 'enum'");

    std::vector<ast::EnumMember> members;
    if (check(TokenType::RightBrace)) {
        report("an enum must contain at least one member", peek().location);
        throw ParseFailure {};
    }

    for (;;) {
        const Token& member = consume(TokenType::Identifier, "expected enum member name");
        members.push_back(ast::EnumMember {
            .name = member.lexeme,
            .span = ast::SourceSpan {.begin = member.location, .end = peek().location},
        });

        if (!match(TokenType::Comma)) {
            break;
        }
        if (check(TokenType::RightBrace)) {
            break; // Trailing comma is accepted.
        }
    }

    (void)consume(TokenType::RightBrace, "expected '}' after enum members");
    const Token& name = consume(TokenType::Identifier, "expected enum type name after '}'");
    (void)consume(TokenType::Semicolon, "expected ';' after enum declaration");

    const ast::SourceSpan span = spanFrom(begin);
    return std::make_unique<ast::EnumDecl>(name.lexeme, std::move(members), span);
}

ast::VariableSpec Parser::parseVariableSpec(bool allowMutability, bool consumeSemicolon) {
    const SourceLocation begin = peek().location;
    bool isMutable = false;
    if (allowMutability && match(TokenType::KwMut)) {
        isMutable = true;
    }

    ast::TypeRef type = parseType();
    const Token& name = consume(TokenType::Identifier, "expected variable name after type");

    ast::ExprPtr initializer;
    if (match(TokenType::Assign)) {
        initializer = parseInitializer();
    }

    if (consumeSemicolon) {
        (void)consume(TokenType::Semicolon, "expected ';' after variable declaration");
    }
    return ast::VariableSpec {
        .type = std::move(type),
        .name = name.lexeme,
        .isMutable = isMutable,
        .initializer = std::move(initializer),
        .span = spanFrom(begin),
    };
}

ast::TypeRef Parser::parseType() {
    const Token token = peek();
    if (!isTypeStart(token)) {
        report("expected a type name", token.location);
        throw ParseFailure {};
    }
    (void)advance();
    return ast::TypeRef {
        .name = token.lexeme,
        .span = spanFrom(token.location),
    };
}

ast::Parameter Parser::parseParameter(bool allowMissingName) {
    const SourceLocation begin = peek().location;
    ast::TypeRef type = parseType();
    std::optional<std::string> name;
    if (check(TokenType::Identifier)) {
        name = advance().lexeme;
    } else if (!allowMissingName) {
        report("expected parameter name", peek().location);
        throw ParseFailure {};
    }

    return ast::Parameter {
        .type = std::move(type),
        .name = std::move(name),
        .span = spanFrom(begin),
    };
}

std::vector<ast::Parameter> Parser::parseFunctionParameters(
    bool allowMissingNames,
    bool signatureOnly)
{
    std::vector<ast::Parameter> parameters;
    if (check(TokenType::RightParen)) {
        return parameters;
    }

    for (;;) {
        if (signatureOnly) {
            const SourceLocation begin = peek().location;
            const Token& name = consume(TokenType::Identifier, "expected parameter name");
            parameters.push_back(ast::Parameter {
                .type = std::nullopt,
                .name = name.lexeme,
                .span = spanFrom(begin),
            });
        } else {
            parameters.push_back(parseParameter(allowMissingNames));
        }

        if (!match(TokenType::Comma)) {
            break;
        }
    }
    return parameters;
}

ast::StmtPtr Parser::parseStatement() {
    if (check(TokenType::LeftBrace)) {
        return parseBlock();
    }
    if (check(TokenType::KwMut) || looksLikeLocalVariableDeclaration()) {
        return parseVariableDeclarationStatement();
    }

    switch (peek().type) {
    case TokenType::KwReturn:
        return parseReturnStatement();
    case TokenType::KwIf:
        return parseIfStatement();
    case TokenType::KwWhile:
        return parseWhileStatement();
    case TokenType::KwFor:
        return parseForStatement();
    case TokenType::KwBreak: {
        const SourceLocation begin = advance().location;
        (void)consume(TokenType::Semicolon, "expected ';' after 'break'");
        return std::make_unique<ast::BreakStmt>(spanFrom(begin));
    }
    case TokenType::KwContinue: {
        const SourceLocation begin = advance().location;
        (void)consume(TokenType::Semicolon, "expected ';' after 'continue'");
        return std::make_unique<ast::ContinueStmt>(spanFrom(begin));
    }
    default:
        return parseExpressionOrAssignmentStatement();
    }
}

std::unique_ptr<ast::BlockStmt> Parser::parseBlock() {
    const SourceLocation begin = consume(TokenType::LeftBrace, "expected '{' to begin block").location;
    std::vector<ast::StmtPtr> statements;

    while (!check(TokenType::RightBrace) && !atEnd()) {
        const std::size_t before = current_;
        try {
            statements.push_back(parseStatement());
        } catch (const ParseFailure&) {
            synchronizeStatement();
        }
        if (current_ == before && !check(TokenType::RightBrace) && !atEnd()) {
            (void)advance();
        }
    }

    (void)consume(TokenType::RightBrace, "expected '}' to close block");
    return std::make_unique<ast::BlockStmt>(std::move(statements), spanFrom(begin));
}

ast::StmtPtr Parser::parseVariableDeclarationStatement(bool consumeSemicolon) {
    const SourceLocation begin = peek().location;
    ast::VariableSpec variable = parseVariableSpec(true, consumeSemicolon);
    const ast::SourceSpan span = spanFrom(begin);
    variable.span = span;
    return std::make_unique<ast::VariableDeclStmt>(std::move(variable), span);
}

ast::StmtPtr Parser::parseExpressionOrAssignmentStatement(bool consumeSemicolon) {
    const SourceLocation begin = peek().location;
    ast::ExprPtr expression = parseExpression();

    if (isAssignmentOperator(peek().type)) {
        if (!isAssignable(*expression)) {
            report("left side of an assignment must be a variable or struct field", expression->span().begin);
            throw ParseFailure {};
        }

        const TokenType operatorToken = advance().type;
        const ast::AssignmentOperator op = assignmentOperator(operatorToken);
        ast::ExprPtr value = parseInitializer();
        if (consumeSemicolon) {
            (void)consume(TokenType::Semicolon, "expected ';' after assignment");
        }
        return std::make_unique<ast::AssignmentStmt>(
            std::move(expression), op, std::move(value), spanFrom(begin));
    }

    if (dynamic_cast<ast::CallExpr*>(expression.get()) == nullptr) {
        report("only function calls and assignments are valid expression statements", expression->span().begin);
        throw ParseFailure {};
    }

    if (consumeSemicolon) {
        (void)consume(TokenType::Semicolon, "expected ';' after function call");
    }
    return std::make_unique<ast::ExpressionStmt>(std::move(expression), spanFrom(begin));
}

ast::StmtPtr Parser::parseReturnStatement() {
    const SourceLocation begin = consume(TokenType::KwReturn, "expected 'return'").location;
    ast::ExprPtr value = parseExpression();
    (void)consume(TokenType::Semicolon, "expected ';' after return value");
    return std::make_unique<ast::ReturnStmt>(std::move(value), spanFrom(begin));
}

ast::StmtPtr Parser::parseIfStatement() {
    const SourceLocation begin = consume(TokenType::KwIf, "expected 'if'").location;
    (void)consume(TokenType::LeftParen, "expected '(' after 'if'");
    ast::ExprPtr condition = parseExpression();
    (void)consume(TokenType::RightParen, "expected ')' after if condition");
    auto thenBranch = parseBlock();

    ast::StmtPtr elseBranch;
    if (match(TokenType::KwElse)) {
        if (check(TokenType::KwIf)) {
            elseBranch = parseIfStatement();
        } else {
            elseBranch = parseBlock();
        }
    }

    return std::make_unique<ast::IfStmt>(
        std::move(condition), std::move(thenBranch), std::move(elseBranch), spanFrom(begin));
}

ast::StmtPtr Parser::parseWhileStatement() {
    const SourceLocation begin = consume(TokenType::KwWhile, "expected 'while'").location;
    (void)consume(TokenType::LeftParen, "expected '(' after 'while'");
    ast::ExprPtr condition = parseExpression();
    (void)consume(TokenType::RightParen, "expected ')' after while condition");
    auto body = parseBlock();
    return std::make_unique<ast::WhileStmt>(std::move(condition), std::move(body), spanFrom(begin));
}

ast::StmtPtr Parser::parseForStatement() {
    const SourceLocation begin = consume(TokenType::KwFor, "expected 'for'").location;
    (void)consume(TokenType::LeftParen, "expected '(' after 'for'");

    ast::StmtPtr initializer;
    if (!check(TokenType::Semicolon)) {
        if (check(TokenType::KwMut) || looksLikeLocalVariableDeclaration()) {
            initializer = parseVariableDeclarationStatement(false);
        } else {
            initializer = parseExpressionOrAssignmentStatement(false);
        }
    }
    (void)consume(TokenType::Semicolon, "expected ';' after for-loop initializer");

    ast::ExprPtr condition;
    if (!check(TokenType::Semicolon)) {
        condition = parseExpression();
    }
    (void)consume(TokenType::Semicolon, "expected ';' after for-loop condition");

    ast::StmtPtr increment;
    if (!check(TokenType::RightParen)) {
        increment = parseExpressionOrAssignmentStatement(false);
    }
    (void)consume(TokenType::RightParen, "expected ')' after for-loop clauses");

    auto body = parseBlock();
    return std::make_unique<ast::ForStmt>(
        std::move(initializer),
        std::move(condition),
        std::move(increment),
        std::move(body),
        spanFrom(begin));
}

ast::ExprPtr Parser::parseInitializer() {
    if (check(TokenType::LeftBrace)) {
        return parseStructInitializer();
    }
    return parseExpression();
}

ast::ExprPtr Parser::parseStructInitializer() {
    const SourceLocation begin = consume(TokenType::LeftBrace, "expected '{' to begin struct initializer").location;
    std::vector<ast::DesignatedInitializer> fields;

    if (!check(TokenType::RightBrace)) {
        for (;;) {
            const SourceLocation fieldBegin = peek().location;
            (void)consume(TokenType::Dot, "expected '.' before designated field name");
            const Token& name = consume(TokenType::Identifier, "expected field name after '.'");
            (void)consume(TokenType::Assign, "expected '=' after designated field name");
            ast::ExprPtr value = parseInitializer();
            fields.push_back(ast::DesignatedInitializer {
                .fieldName = name.lexeme,
                .value = std::move(value),
                .span = spanFrom(fieldBegin),
            });

            if (!match(TokenType::Comma)) {
                break;
            }
            if (check(TokenType::RightBrace)) {
                break; // Trailing comma is accepted.
            }
        }
    }

    (void)consume(TokenType::RightBrace, "expected '}' after struct initializer");
    return std::make_unique<ast::StructInitializerExpr>(std::move(fields), spanFrom(begin));
}

ast::ExprPtr Parser::parseExpression() {
    return parseBinaryExpression(1);
}

ast::ExprPtr Parser::parseBinaryExpression(int minimumPrecedence) {
    ast::ExprPtr left = parseUnaryExpression();

    for (;;) {
        const int currentPrecedence = precedence(peek().type);
        if (currentPrecedence < minimumPrecedence) {
            break;
        }

        const TokenType operatorToken = advance().type;
        ast::ExprPtr right = parseBinaryExpression(currentPrecedence + 1);
        const SourceLocation begin = left->span().begin;
        left = std::make_unique<ast::BinaryExpr>(
            std::move(left),
            binaryOperator(operatorToken),
            std::move(right),
            spanFrom(begin));
    }

    return left;
}

ast::ExprPtr Parser::parseUnaryExpression() {
    if (check(TokenType::Plus) || check(TokenType::Minus) ||
        check(TokenType::LogicalNot) || check(TokenType::BitNot)) {
        const Token operatorToken = advance();
        ast::ExprPtr operand = parseUnaryExpression();
        return std::make_unique<ast::UnaryExpr>(
            unaryOperator(operatorToken.type),
            std::move(operand),
            spanFrom(operatorToken.location));
    }
    return parsePostfixExpression();
}

ast::ExprPtr Parser::parsePostfixExpression() {
    ast::ExprPtr expression = parsePrimaryExpression();

    while (match(TokenType::Dot)) {
        const Token& member = consume(TokenType::Identifier, "expected member name after '.'");
        const SourceLocation begin = expression->span().begin;
        expression = std::make_unique<ast::MemberAccessExpr>(
            std::move(expression), member.lexeme, spanFrom(begin));
    }

    return expression;
}

ast::ExprPtr Parser::parsePrimaryExpression() {
    const Token token = peek();

    switch (token.type) {
    case TokenType::IntegerLiteral:
        (void)advance();
        return std::make_unique<ast::IntegerLiteralExpr>(token.lexeme, spanFrom(token.location));
    case TokenType::FloatingLiteral:
        (void)advance();
        return std::make_unique<ast::FloatingLiteralExpr>(token.lexeme, spanFrom(token.location));
    case TokenType::StringLiteral:
        (void)advance();
        return std::make_unique<ast::StringLiteralExpr>(
            token.stringValue.value_or(std::string {}), spanFrom(token.location));
    case TokenType::KwTrue:
        (void)advance();
        return std::make_unique<ast::BooleanLiteralExpr>(true, spanFrom(token.location));
    case TokenType::KwFalse:
        (void)advance();
        return std::make_unique<ast::BooleanLiteralExpr>(false, spanFrom(token.location));
    case TokenType::KwNull:
        (void)advance();
        return std::make_unique<ast::NullLiteralExpr>(spanFrom(token.location));
    case TokenType::Identifier: {
        const Token& name = advance();
        if (check(TokenType::LeftParen)) {
            return parseFunctionCall(name);
        }
        return std::make_unique<ast::IdentifierExpr>(name.lexeme, spanFrom(name.location));
    }
    case TokenType::LeftParen: {
        (void)advance();
        ast::ExprPtr expression = parseExpression();
        (void)consume(TokenType::RightParen, "expected ')' after expression");
        return expression;
    }
    default:
        report("expected an expression", token.location);
        throw ParseFailure {};
    }
}

ast::ExprPtr Parser::parseFunctionCall(const Token& nameToken) {
    (void)consume(TokenType::LeftParen, "expected '(' after function name");
    std::vector<ast::ExprPtr> arguments = parseArguments();
    (void)consume(TokenType::RightParen, "expected ')' after function arguments");
    return std::make_unique<ast::CallExpr>(
        nameToken.lexeme, std::move(arguments), spanFrom(nameToken.location));
}

std::vector<ast::ExprPtr> Parser::parseArguments() {
    std::vector<ast::ExprPtr> arguments;
    if (check(TokenType::RightParen)) {
        return arguments;
    }

    for (;;) {
        arguments.push_back(parseExpression());
        if (!match(TokenType::Comma)) {
            break;
        }
    }
    return arguments;
}

bool Parser::isTypeStart(const Token& token) const noexcept {
    switch (token.type) {
    case TokenType::Identifier:
    case TokenType::KwInt:
    case TokenType::KwFloat:
    case TokenType::KwDouble:
    case TokenType::KwBool:
    case TokenType::KwString:
    case TokenType::KwAny:
    case TokenType::KwStream:
    case TokenType::KwVoid:
        return true;
    default:
        return false;
    }
}

bool Parser::looksLikeLocalVariableDeclaration() const noexcept {
    if (isTypeStart(peek()) && peek().type != TokenType::Identifier) {
        return true;
    }
    return check(TokenType::Identifier) && lookahead(1).type == TokenType::Identifier;
}

bool Parser::looksLikeSignatureImplementation() const noexcept {
    if (!check(TokenType::Identifier) || lookahead(1).type != TokenType::LeftParen) {
        return false;
    }

    std::size_t distance = 2;
    if (lookahead(distance).type != TokenType::RightParen) {
        for (;;) {
            if (lookahead(distance).type != TokenType::Identifier) {
                return false;
            }
            ++distance;
            if (lookahead(distance).type != TokenType::Comma) {
                break;
            }
            ++distance;
        }
    }

    return lookahead(distance).type == TokenType::RightParen &&
           lookahead(distance + 1).type == TokenType::FatArrow;
}

bool Parser::isAssignmentOperator(TokenType type) const noexcept {
    switch (type) {
    case TokenType::Assign:
    case TokenType::PlusEqual:
    case TokenType::MinusEqual:
    case TokenType::StarEqual:
    case TokenType::SlashEqual:
    case TokenType::ModuloEqual:
        return true;
    default:
        return false;
    }
}

int Parser::precedence(TokenType type) const noexcept {
    switch (type) {
    case TokenType::LogicalOr: return 1;
    case TokenType::LogicalAnd: return 2;
    case TokenType::BitOr: return 3;
    case TokenType::BitXor: return 4;
    case TokenType::BitAnd: return 5;
    case TokenType::EqualEqual:
    case TokenType::NotEqual: return 6;
    case TokenType::Less:
    case TokenType::Greater:
    case TokenType::LessEqual:
    case TokenType::GreaterEqual: return 7;
    case TokenType::ShiftLeft:
    case TokenType::ShiftRight: return 8;
    case TokenType::Plus:
    case TokenType::Minus: return 9;
    case TokenType::Star:
    case TokenType::Slash:
    case TokenType::Modulo: return 10;
    default: return 0;
    }
}

ast::BinaryOperator Parser::binaryOperator(TokenType type) const {
    switch (type) {
    case TokenType::Plus: return ast::BinaryOperator::Add;
    case TokenType::Minus: return ast::BinaryOperator::Subtract;
    case TokenType::Star: return ast::BinaryOperator::Multiply;
    case TokenType::Slash: return ast::BinaryOperator::Divide;
    case TokenType::Modulo: return ast::BinaryOperator::Modulo;
    case TokenType::EqualEqual: return ast::BinaryOperator::Equal;
    case TokenType::NotEqual: return ast::BinaryOperator::NotEqual;
    case TokenType::Less: return ast::BinaryOperator::Less;
    case TokenType::Greater: return ast::BinaryOperator::Greater;
    case TokenType::LessEqual: return ast::BinaryOperator::LessEqual;
    case TokenType::GreaterEqual: return ast::BinaryOperator::GreaterEqual;
    case TokenType::LogicalAnd: return ast::BinaryOperator::LogicalAnd;
    case TokenType::LogicalOr: return ast::BinaryOperator::LogicalOr;
    case TokenType::BitAnd: return ast::BinaryOperator::BitwiseAnd;
    case TokenType::BitOr: return ast::BinaryOperator::BitwiseOr;
    case TokenType::BitXor: return ast::BinaryOperator::BitwiseXor;
    case TokenType::ShiftLeft: return ast::BinaryOperator::ShiftLeft;
    case TokenType::ShiftRight: return ast::BinaryOperator::ShiftRight;
    default:
        return ast::BinaryOperator::Add;
    }
}

ast::AssignmentOperator Parser::assignmentOperator(TokenType type) const {
    switch (type) {
    case TokenType::Assign: return ast::AssignmentOperator::Assign;
    case TokenType::PlusEqual: return ast::AssignmentOperator::Add;
    case TokenType::MinusEqual: return ast::AssignmentOperator::Subtract;
    case TokenType::StarEqual: return ast::AssignmentOperator::Multiply;
    case TokenType::SlashEqual: return ast::AssignmentOperator::Divide;
    case TokenType::ModuloEqual: return ast::AssignmentOperator::Modulo;
    default: return ast::AssignmentOperator::Assign;
    }
}

ast::UnaryOperator Parser::unaryOperator(TokenType type) const {
    switch (type) {
    case TokenType::Plus: return ast::UnaryOperator::Plus;
    case TokenType::Minus: return ast::UnaryOperator::Minus;
    case TokenType::LogicalNot: return ast::UnaryOperator::LogicalNot;
    case TokenType::BitNot: return ast::UnaryOperator::BitwiseNot;
    default: return ast::UnaryOperator::Plus;
    }
}

bool Parser::isAssignable(const ast::Expression& expression) const noexcept {
    return dynamic_cast<const ast::IdentifierExpr*>(&expression) != nullptr ||
           dynamic_cast<const ast::MemberAccessExpr*>(&expression) != nullptr;
}

} // namespace serenity::parser
