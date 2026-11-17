#pragma once

#include "serenity/lexer/token.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace serenity::ast {

// Token locations refer to the preprocessed source buffer. The preprocessor's
// source map can translate span.begin.offset back to the original .sn/.snh file.
// End positions are exclusive.
struct SourceSpan {
    SourceLocation begin {};
    SourceLocation end {};
};

class Node {
public:
    explicit Node(SourceSpan span = {}) noexcept
        : span_(span) {}
    virtual ~Node() = default;

    [[nodiscard]] const SourceSpan& span() const noexcept { return span_; }

private:
    SourceSpan span_;
};

class Expression;
class Statement;
class Declaration;
class BlockStmt;
class CallExpr;

using ExprPtr = std::unique_ptr<Expression>;
using StmtPtr = std::unique_ptr<Statement>;
using DeclPtr = std::unique_ptr<Declaration>;

// A syntactic type name. Whether it denotes a primitive, struct, enum, or an
// unknown type is deliberately resolved by semantic analysis, not the parser.
struct TypeRef {
    std::string name;
    SourceSpan span {};
};

class Expression : public Node {
public:
    using Node::Node;
    ~Expression() override = default;
};

class Statement : public Node {
public:
    using Node::Node;
    ~Statement() override = default;
};

class Declaration : public Node {
public:
    using Node::Node;
    ~Declaration() override = default;
};

// -----------------------------------------------------------------------------
// Expressions
// -----------------------------------------------------------------------------

class IntegerLiteralExpr final : public Expression {
public:
    IntegerLiteralExpr(std::string spelling, SourceSpan span = {})
        : Expression(span), spelling_(std::move(spelling)) {}

    // Keep the original decimal spelling. Range checking and conversion belong
    // to semantic analysis, where the target integer type is known.
    [[nodiscard]] const std::string& spelling() const noexcept { return spelling_; }

private:
    std::string spelling_;
};

class FloatingLiteralExpr final : public Expression {
public:
    FloatingLiteralExpr(std::string spelling, SourceSpan span = {})
        : Expression(span), spelling_(std::move(spelling)) {}

    // The parser does not decide whether this literal initializes float/double.
    [[nodiscard]] const std::string& spelling() const noexcept { return spelling_; }

private:
    std::string spelling_;
};

class StringLiteralExpr final : public Expression {
public:
    StringLiteralExpr(std::string value, SourceSpan span = {})
        : Expression(span), value_(std::move(value)) {}

    // This stores the decoded stringValue supplied by the lexer.
    [[nodiscard]] const std::string& value() const noexcept { return value_; }

private:
    std::string value_;
};

class BooleanLiteralExpr final : public Expression {
public:
    BooleanLiteralExpr(bool value, SourceSpan span = {})
        : Expression(span), value_(value) {}

    [[nodiscard]] bool value() const noexcept { return value_; }

private:
    bool value_;
};

class NullLiteralExpr final : public Expression {
public:
    using Expression::Expression;
};

class IdentifierExpr final : public Expression {
public:
    IdentifierExpr(std::string name, SourceSpan span = {})
        : Expression(span), name_(std::move(name)) {}

    [[nodiscard]] const std::string& name() const noexcept { return name_; }

private:
    std::string name_;
};

enum class UnaryOperator {
    Plus,
    Minus,
    LogicalNot,
    BitwiseNot,
};

class UnaryExpr final : public Expression {
public:
    UnaryExpr(UnaryOperator op, ExprPtr operand, SourceSpan span = {})
        : Expression(span), op_(op), operand_(std::move(operand)) {}

    [[nodiscard]] UnaryOperator op() const noexcept { return op_; }
    [[nodiscard]] const Expression& operand() const noexcept { return *operand_; }
    [[nodiscard]] Expression& operand() noexcept { return *operand_; }

private:
    UnaryOperator op_;
    ExprPtr operand_;
};

enum class BinaryOperator {
    Add,
    Subtract,
    Multiply,
    Divide,
    Modulo,

    Equal,
    NotEqual,
    Less,
    Greater,
    LessEqual,
    GreaterEqual,

    LogicalAnd,
    LogicalOr,

    BitwiseAnd,
    BitwiseOr,
    BitwiseXor,
    ShiftLeft,
    ShiftRight,
};

class BinaryExpr final : public Expression {
public:
    BinaryExpr(ExprPtr left, BinaryOperator op, ExprPtr right, SourceSpan span = {})
        : Expression(span), left_(std::move(left)), op_(op), right_(std::move(right)) {}

    [[nodiscard]] const Expression& left() const noexcept { return *left_; }
    [[nodiscard]] Expression& left() noexcept { return *left_; }
    [[nodiscard]] BinaryOperator op() const noexcept { return op_; }
    [[nodiscard]] const Expression& right() const noexcept { return *right_; }
    [[nodiscard]] Expression& right() noexcept { return *right_; }

private:
    ExprPtr left_;
    BinaryOperator op_;
    ExprPtr right_;
};

class MemberAccessExpr final : public Expression {
public:
    MemberAccessExpr(ExprPtr object, std::string member, SourceSpan span = {})
        : Expression(span), object_(std::move(object)), member_(std::move(member)) {}

    [[nodiscard]] const Expression& object() const noexcept { return *object_; }
    [[nodiscard]] Expression& object() noexcept { return *object_; }
    [[nodiscard]] const std::string& member() const noexcept { return member_; }

private:
    ExprPtr object_;
    std::string member_;
};

class CallExpr final : public Expression {
public:
    CallExpr(std::string callee, std::vector<ExprPtr> arguments, SourceSpan span = {})
        : Expression(span), callee_(std::move(callee)), arguments_(std::move(arguments)) {}

    [[nodiscard]] const std::string& callee() const noexcept { return callee_; }
    [[nodiscard]] const std::vector<ExprPtr>& arguments() const noexcept { return arguments_; }
    [[nodiscard]] std::vector<ExprPtr>& arguments() noexcept { return arguments_; }

private:
    std::string callee_;
    std::vector<ExprPtr> arguments_;
};

struct DesignatedInitializer {
    std::string fieldName;
    ExprPtr value;
    SourceSpan span {};
};

class StructInitializerExpr final : public Expression {
public:
    explicit StructInitializerExpr(
        std::vector<DesignatedInitializer> fields,
        SourceSpan span = {})
        : Expression(span), fields_(std::move(fields)) {}

    [[nodiscard]] const std::vector<DesignatedInitializer>& fields() const noexcept { return fields_; }
    [[nodiscard]] std::vector<DesignatedInitializer>& fields() noexcept { return fields_; }

private:
    std::vector<DesignatedInitializer> fields_;
};

// -----------------------------------------------------------------------------
// Variable and field descriptions
// -----------------------------------------------------------------------------

struct VariableSpec {
    TypeRef type;
    std::string name;
    bool isMutable {false};
    ExprPtr initializer; // null means use the language-defined default value
    SourceSpan span {};
};

struct FieldDecl {
    TypeRef type;
    std::string name;
    ExprPtr defaultValue; // null means no explicit field default
    SourceSpan span {};
};

// -----------------------------------------------------------------------------
// Statements
// -----------------------------------------------------------------------------

class BlockStmt final : public Statement {
public:
    explicit BlockStmt(std::vector<StmtPtr> statements = {}, SourceSpan span = {})
        : Statement(span), statements_(std::move(statements)) {}

    [[nodiscard]] const std::vector<StmtPtr>& statements() const noexcept { return statements_; }
    [[nodiscard]] std::vector<StmtPtr>& statements() noexcept { return statements_; }

private:
    std::vector<StmtPtr> statements_;
};

class VariableDeclStmt final : public Statement {
public:
    explicit VariableDeclStmt(VariableSpec variable, SourceSpan span = {})
        : Statement(span), variable_(std::move(variable)) {}

    [[nodiscard]] const VariableSpec& variable() const noexcept { return variable_; }
    [[nodiscard]] VariableSpec& variable() noexcept { return variable_; }

private:
    VariableSpec variable_;
};

class ExpressionStmt final : public Statement {
public:
    explicit ExpressionStmt(ExprPtr expression, SourceSpan span = {})
        : Statement(span), expression_(std::move(expression)) {}

    [[nodiscard]] const Expression& expression() const noexcept { return *expression_; }
    [[nodiscard]] Expression& expression() noexcept { return *expression_; }

private:
    ExprPtr expression_;
};

enum class AssignmentOperator {
    Assign,
    Add,
    Subtract,
    Multiply,
    Divide,
    Modulo,
};

class AssignmentStmt final : public Statement {
public:
    AssignmentStmt(
        ExprPtr target,
        AssignmentOperator op,
        ExprPtr value,
        SourceSpan span = {})
        : Statement(span), target_(std::move(target)), op_(op), value_(std::move(value)) {}

    // The parser accepts identifier/member-access targets. Semantic analysis
    // verifies that the target exists and is mutable.
    [[nodiscard]] const Expression& target() const noexcept { return *target_; }
    [[nodiscard]] Expression& target() noexcept { return *target_; }
    [[nodiscard]] AssignmentOperator op() const noexcept { return op_; }
    [[nodiscard]] const Expression& value() const noexcept { return *value_; }
    [[nodiscard]] Expression& value() noexcept { return *value_; }

private:
    ExprPtr target_;
    AssignmentOperator op_;
    ExprPtr value_;
};

class ReturnStmt final : public Statement {
public:
    explicit ReturnStmt(ExprPtr value, SourceSpan span = {})
        : Statement(span), value_(std::move(value)) {}

    [[nodiscard]] const Expression& value() const noexcept { return *value_; }
    [[nodiscard]] Expression& value() noexcept { return *value_; }

private:
    ExprPtr value_;
};

class IfStmt final : public Statement {
public:
    IfStmt(
        ExprPtr condition,
        std::unique_ptr<BlockStmt> thenBranch,
        StmtPtr elseBranch = nullptr,
        SourceSpan span = {})
        : Statement(span),
          condition_(std::move(condition)),
          thenBranch_(std::move(thenBranch)),
          elseBranch_(std::move(elseBranch)) {}

    [[nodiscard]] const Expression& condition() const noexcept { return *condition_; }
    [[nodiscard]] const BlockStmt& thenBranch() const noexcept { return *thenBranch_; }
    [[nodiscard]] BlockStmt& thenBranch() noexcept { return *thenBranch_; }
    [[nodiscard]] const Statement* elseBranch() const noexcept { return elseBranch_.get(); }
    [[nodiscard]] Statement* elseBranch() noexcept { return elseBranch_.get(); }

private:
    ExprPtr condition_;
    std::unique_ptr<BlockStmt> thenBranch_;
    StmtPtr elseBranch_;
};

class WhileStmt final : public Statement {
public:
    WhileStmt(ExprPtr condition, std::unique_ptr<BlockStmt> body, SourceSpan span = {})
        : Statement(span), condition_(std::move(condition)), body_(std::move(body)) {}

    [[nodiscard]] const Expression& condition() const noexcept { return *condition_; }
    [[nodiscard]] const BlockStmt& body() const noexcept { return *body_; }
    [[nodiscard]] BlockStmt& body() noexcept { return *body_; }

private:
    ExprPtr condition_;
    std::unique_ptr<BlockStmt> body_;
};

class ForStmt final : public Statement {
public:
    ForStmt(
        StmtPtr initializer,
        ExprPtr condition,
        StmtPtr increment,
        std::unique_ptr<BlockStmt> body,
        SourceSpan span = {})
        : Statement(span),
          initializer_(std::move(initializer)),
          condition_(std::move(condition)),
          increment_(std::move(increment)),
          body_(std::move(body)) {}

    [[nodiscard]] const Statement* initializer() const noexcept { return initializer_.get(); }
    [[nodiscard]] Statement* initializer() noexcept { return initializer_.get(); }
    [[nodiscard]] const Expression* condition() const noexcept { return condition_.get(); }
    [[nodiscard]] Expression* condition() noexcept { return condition_.get(); }
    [[nodiscard]] const Statement* increment() const noexcept { return increment_.get(); }
    [[nodiscard]] Statement* increment() noexcept { return increment_.get(); }
    [[nodiscard]] const BlockStmt& body() const noexcept { return *body_; }
    [[nodiscard]] BlockStmt& body() noexcept { return *body_; }

private:
    StmtPtr initializer_;
    ExprPtr condition_;
    StmtPtr increment_;
    std::unique_ptr<BlockStmt> body_;
};

class BreakStmt final : public Statement {
public:
    using Statement::Statement;
};

class ContinueStmt final : public Statement {
public:
    using Statement::Statement;
};

// -----------------------------------------------------------------------------
// Declarations
// -----------------------------------------------------------------------------

class GlobalVariableDecl final : public Declaration {
public:
    explicit GlobalVariableDecl(VariableSpec variable, SourceSpan span = {})
        : Declaration(span), variable_(std::move(variable)) {}

    [[nodiscard]] const VariableSpec& variable() const noexcept { return variable_; }
    [[nodiscard]] VariableSpec& variable() noexcept { return variable_; }

private:
    VariableSpec variable_;
};

struct Parameter {
    // Prototype parameters have a type and may optionally have a name.
    // Typed definitions have both type and name.
    // Signature-based definitions (add(a, b) => int { ... }) have a name
    // but no type; semantic analysis obtains the types from the prototype.
    std::optional<TypeRef> type;
    std::optional<std::string> name;
    SourceSpan span {};
};

enum class FunctionForm {
    Prototype,
    TypedDefinition,
    SignatureImplementation,
};

class FunctionDecl final : public Declaration {
public:
    FunctionDecl(
        std::string name,
        std::vector<Parameter> parameters,
        TypeRef returnType,
        FunctionForm form,
        std::unique_ptr<BlockStmt> body = nullptr,
        SourceSpan span = {})
        : Declaration(span),
          name_(std::move(name)),
          parameters_(std::move(parameters)),
          returnType_(std::move(returnType)),
          form_(form),
          body_(std::move(body)) {}

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const std::vector<Parameter>& parameters() const noexcept { return parameters_; }
    [[nodiscard]] std::vector<Parameter>& parameters() noexcept { return parameters_; }
    [[nodiscard]] const TypeRef& returnType() const noexcept { return returnType_; }
    [[nodiscard]] FunctionForm form() const noexcept { return form_; }
    [[nodiscard]] bool isPrototype() const noexcept { return form_ == FunctionForm::Prototype; }
    [[nodiscard]] const BlockStmt* body() const noexcept { return body_.get(); }
    [[nodiscard]] BlockStmt* body() noexcept { return body_.get(); }

private:
    std::string name_;
    std::vector<Parameter> parameters_;
    TypeRef returnType_;
    FunctionForm form_;
    std::unique_ptr<BlockStmt> body_;
};

class StructDecl final : public Declaration {
public:
    StructDecl(std::string name, std::vector<FieldDecl> fields, SourceSpan span = {})
        : Declaration(span), name_(std::move(name)), fields_(std::move(fields)) {}

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const std::vector<FieldDecl>& fields() const noexcept { return fields_; }
    [[nodiscard]] std::vector<FieldDecl>& fields() noexcept { return fields_; }

private:
    std::string name_;
    std::vector<FieldDecl> fields_;
};

struct EnumMember {
    std::string name;
    SourceSpan span {};
};

class EnumDecl final : public Declaration {
public:
    EnumDecl(std::string name, std::vector<EnumMember> members, SourceSpan span = {})
        : Declaration(span), name_(std::move(name)), members_(std::move(members)) {}

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const std::vector<EnumMember>& members() const noexcept { return members_; }

private:
    std::string name_;
    std::vector<EnumMember> members_;
};

class Program final : public Node {
public:
    explicit Program(std::vector<DeclPtr> declarations = {}, SourceSpan span = {})
        : Node(span), declarations_(std::move(declarations)) {}

    [[nodiscard]] const std::vector<DeclPtr>& declarations() const noexcept { return declarations_; }
    [[nodiscard]] std::vector<DeclPtr>& declarations() noexcept { return declarations_; }

private:
    std::vector<DeclPtr> declarations_;
};

} // namespace serenity::ast
