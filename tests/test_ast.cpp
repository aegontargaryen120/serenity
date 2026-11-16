#include "serenity/ast/ast.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

serenity::ast::SourceSpan makeSpan(std::size_t begin, std::size_t end) {
    return {
        .begin = serenity::SourceLocation{begin, 1, begin + 1},
        .end = serenity::SourceLocation{end, 1, end + 1},
    };
}

} // namespace

int main() {
    using namespace serenity::ast;

    const auto intType = TypeRef{"int", makeSpan(0, 3)};
    auto initialValue = std::make_unique<IntegerLiteralExpr>("17", makeSpan(12, 14));

    VariableSpec global {
        .type = intType,
        .name = "globalCount",
        .isMutable = true,
        .initializer = std::move(initialValue),
        .span = makeSpan(0, 15),
    };

    std::vector<StmtPtr> bodyStatements;
    bodyStatements.push_back(std::make_unique<VariableDeclStmt>(VariableSpec{
        .type = TypeRef{"bool", makeSpan(30, 34)},
        .name = "enabled",
        .isMutable = false,
        .initializer = std::make_unique<BooleanLiteralExpr>(false, makeSpan(44, 49)),
        .span = makeSpan(30, 50),
    }));

    std::vector<ExprPtr> args;
    args.push_back(std::make_unique<StringLiteralExpr>("Serenity", makeSpan(65, 75)));
    bodyStatements.push_back(std::make_unique<ExpressionStmt>(
        std::make_unique<CallExpr>("println", std::move(args), makeSpan(57, 76)),
        makeSpan(57, 77)));
    bodyStatements.push_back(std::make_unique<ReturnStmt>(
        std::make_unique<IntegerLiteralExpr>("0", makeSpan(88, 89)),
        makeSpan(81, 90)));

    auto functionBody = std::make_unique<BlockStmt>(std::move(bodyStatements), makeSpan(55, 92));
    std::vector<Parameter> parameters;
    auto mainFunction = std::make_unique<FunctionDecl>(
        "main",
        std::move(parameters),
        TypeRef{"int", makeSpan(49, 52)},
        FunctionForm::TypedDefinition,
        std::move(functionBody),
        makeSpan(39, 92));

    std::vector<DeclPtr> declarations;
    declarations.push_back(std::make_unique<GlobalVariableDecl>(std::move(global), makeSpan(0, 15)));
    declarations.push_back(std::move(mainFunction));

    Program program(std::move(declarations), makeSpan(0, 92));
    require(program.declarations().size() == 2, "program should own two top-level declarations");

    const auto* globalDecl = dynamic_cast<const GlobalVariableDecl*>(program.declarations()[0].get());
    require(globalDecl != nullptr, "first declaration should be a global variable");
    require(globalDecl->variable().isMutable, "mutability should be preserved");
    require(globalDecl->variable().name == "globalCount", "variable name should be preserved");
    require(globalDecl->variable().initializer != nullptr, "initializer should be owned by the AST");

    const auto* functionDecl = dynamic_cast<const FunctionDecl*>(program.declarations()[1].get());
    require(functionDecl != nullptr, "second declaration should be a function");
    require(functionDecl->name() == "main", "function name should be preserved");
    require(!functionDecl->isPrototype(), "function with a body should not be a prototype");
    require(functionDecl->body() != nullptr, "function body should be present");
    require(functionDecl->body()->statements().size() == 3, "function statements should be retained");

    NullLiteralExpr nullLiteral(makeSpan(100, 104));
    require(nullLiteral.span().begin.offset == 100, "AST nodes should retain source spans");

    std::cout << "All Serenity AST tests passed.\n";
    return EXIT_SUCCESS;
}
