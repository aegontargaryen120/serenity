#pragma once

#include "serenity/ast/ast.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace serenity::semantic {

enum class TypeKind {
    Error,
    Void, // Function result type; void values cannot be stored or passed.
    Null,
    Int,
    Float,
    Double,
    Bool,
    String,
    Any,
    Stream,
    Struct,
    Enum,
};

struct Type {
    TypeKind kind {TypeKind::Error};
    std::string name;

    [[nodiscard]] bool isError() const noexcept { return kind == TypeKind::Error; }
    [[nodiscard]] bool isNumeric() const noexcept;
    [[nodiscard]] bool isInteger() const noexcept;
    [[nodiscard]] bool isNullable() const noexcept;
    [[nodiscard]] std::string toString() const;

    friend bool operator==(const Type& left, const Type& right) noexcept {
        return left.kind == right.kind &&
               ((left.kind != TypeKind::Struct && left.kind != TypeKind::Enum) ||
                left.name == right.name);
    }

    friend bool operator!=(const Type& left, const Type& right) noexcept {
        return !(left == right);
    }
};

struct SemanticError {
    std::string message;
    SourceLocation location {};
};

struct SemanticModel {
    // Expression types are retained for later IR/bytecode generation.
    // Keys point into the Program passed to SemanticAnalyzer and remain valid
    // for as long as that Program remains alive and unmodified.
    std::unordered_map<const ast::Expression*, Type> expressionTypes;
};

struct SemanticResult {
    SemanticModel model;
    std::vector<SemanticError> errors;

    [[nodiscard]] bool ok() const noexcept { return errors.empty(); }
};

// Performs name resolution, declaration validation, type checking, mutability
// checks, function signature matching, and loop/return context validation.
// The analyzer does not modify the AST.
class SemanticAnalyzer {
public:
    explicit SemanticAnalyzer(const ast::Program& program);

    [[nodiscard]] SemanticResult analyze();

private:
    struct VariableSymbol {
        Type type;
        bool isMutable {false};
        ast::SourceSpan declarationSpan {};
    };

    struct FieldSymbol {
        Type type;
        const ast::FieldDecl* declaration {nullptr};
    };

    struct TypeSymbol {
        Type type;
        ast::SourceSpan declarationSpan {};
        std::unordered_map<std::string, FieldSymbol> fields;
        std::unordered_map<std::string, ast::SourceSpan> enumMembers;
    };

    struct FunctionSymbol {
        std::vector<Type> parameterTypes;
        Type returnType;
        bool hasPrototype {false};
        bool hasDefinition {false};
        ast::SourceSpan declarationSpan {};
    };

    const ast::Program& program_;
    SemanticResult result_;

    std::unordered_map<std::string, TypeSymbol> types_;
    std::unordered_map<std::string, FunctionSymbol> functions_;
    std::unordered_map<std::string, VariableSymbol> globals_;
    std::vector<std::unordered_map<std::string, VariableSymbol>> scopes_;

    Type currentReturnType_ {TypeKind::Error, {}};
    bool insideFunction_ {false};
    std::size_t loopDepth_ {0};

    void registerTypeNames();
    void populateTypeMembers();
    void registerFunctionSignatures();
    void registerGlobalVariables();
    void analyzeTypeDefaults();
    void analyzeGlobalInitializers();
    void analyzeFunctionBodies();

    [[nodiscard]] Type resolveType(const ast::TypeRef& typeRef);
    [[nodiscard]] Type primitiveType(const std::string& name) const;
    [[nodiscard]] bool isKnownType(const std::string& name) const;
    [[nodiscard]] bool canAssign(const Type& destination, const Type& source) const noexcept;
    [[nodiscard]] bool isPrintable(const Type& type) const noexcept;

    void analyzeBlock(const ast::BlockStmt& block, bool createScope);
    void analyzeStatement(const ast::Statement& statement);
    void analyzeVariableDeclaration(const ast::VariableSpec& variable);
    void analyzeAssignment(const ast::AssignmentStmt& assignment);
    void analyzeReturn(const ast::ReturnStmt& statement);
    void analyzeIf(const ast::IfStmt& statement);
    void analyzeWhile(const ast::WhileStmt& statement);
    void analyzeFor(const ast::ForStmt& statement);
    void analyzeFunction(const ast::FunctionDecl& function);

    [[nodiscard]] Type analyzeExpression(
        const ast::Expression& expression,
        const Type* expected = nullptr);
    [[nodiscard]] Type analyzeBinaryExpression(
        const ast::BinaryExpr& expression,
        const Type* expected);
    [[nodiscard]] Type analyzeCallExpression(
        const ast::CallExpr& expression,
        const Type* expected);
    [[nodiscard]] Type analyzeMemberAccess(
        const ast::MemberAccessExpr& expression);
    [[nodiscard]] Type analyzeStructInitializer(
        const ast::StructInitializerExpr& expression,
        const Type* expected);
    [[nodiscard]] Type analyzeLValue(const ast::Expression& expression, bool directAssignment);

    [[nodiscard]] VariableSymbol* lookupVariable(const std::string& name);
    [[nodiscard]] const VariableSymbol* lookupVariable(const std::string& name) const;
    [[nodiscard]] const FieldSymbol* lookupField(const Type& type, const std::string& name) const;
    [[nodiscard]] bool isFloatingLiteral(const ast::Expression& expression) const noexcept;
    [[nodiscard]] bool isNumericOperator(ast::BinaryOperator op) const noexcept;
    [[nodiscard]] bool isCompoundNumericOperator(ast::AssignmentOperator op) const noexcept;

    void pushScope();
    void popScope() noexcept;
    void report(std::string message, ast::SourceSpan span);
    void report(std::string message, SourceLocation location);
};

} // namespace serenity::semantic
