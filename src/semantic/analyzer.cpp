#include "serenity/semantic/analyzer.hpp"

#include <algorithm>
#include <utility>

namespace serenity::semantic {

bool Type::isNumeric() const noexcept {
    return kind == TypeKind::Int || kind == TypeKind::Float || kind == TypeKind::Double;
}

bool Type::isInteger() const noexcept {
    return kind == TypeKind::Int;
}

bool Type::isNullable() const noexcept {
    return kind == TypeKind::Null || kind == TypeKind::String || kind == TypeKind::Any ||
           kind == TypeKind::Stream || kind == TypeKind::Struct || kind == TypeKind::Enum;
}

std::string Type::toString() const {
    switch (kind) {
    case TypeKind::Error: return "<error>";
    case TypeKind::Void: return "void";
    case TypeKind::Null: return "null";
    case TypeKind::Int: return "int";
    case TypeKind::Float: return "float";
    case TypeKind::Double: return "double";
    case TypeKind::Bool: return "bool";
    case TypeKind::String: return "string";
    case TypeKind::Any: return "any";
    case TypeKind::Stream: return "stream";
    case TypeKind::Struct:
    case TypeKind::Enum: return name;
    }
    return "<unknown>";
}

SemanticAnalyzer::SemanticAnalyzer(const ast::Program& program)
    : program_(program) {}

SemanticResult SemanticAnalyzer::analyze() {
    registerTypeNames();
    populateTypeMembers();
    registerFunctionSignatures();
    registerGlobalVariables();
    analyzeTypeDefaults();
    analyzeGlobalInitializers();
    analyzeFunctionBodies();
    return std::move(result_);
}

Type SemanticAnalyzer::primitiveType(const std::string& name) const {
    if (name == "int") return {TypeKind::Int, {}};
    if (name == "float") return {TypeKind::Float, {}};
    if (name == "double") return {TypeKind::Double, {}};
    if (name == "bool") return {TypeKind::Bool, {}};
    if (name == "string") return {TypeKind::String, {}};
    if (name == "any") return {TypeKind::Any, {}};
    if (name == "stream") return {TypeKind::Stream, {}};
    if (name == "void") return {TypeKind::Void, {}};
    return {TypeKind::Error, {}};
}

bool SemanticAnalyzer::isKnownType(const std::string& name) const {
    if (!primitiveType(name).isError()) return true;
    return types_.find(name) != types_.end();
}

Type SemanticAnalyzer::resolveType(const ast::TypeRef& typeRef) {
    Type primitive = primitiveType(typeRef.name);
    if (!primitive.isError()) return primitive;

    const auto found = types_.find(typeRef.name);
    if (found != types_.end()) return found->second.type;

    report("unknown type '" + typeRef.name + "'", typeRef.span);
    return {TypeKind::Error, {}};
}

bool SemanticAnalyzer::canAssign(const Type& destination, const Type& source) const noexcept {
    if (destination.isError() || source.isError()) return true;
    if (destination.kind == TypeKind::Any) return true;
    if (source.kind == TypeKind::Any) return destination.kind == TypeKind::Any;
    if (destination == source) return true;
    return source.kind == TypeKind::Null && destination.isNullable();
}

bool SemanticAnalyzer::isPrintable(const Type& type) const noexcept {
    return type.kind == TypeKind::Int || type.kind == TypeKind::Float ||
           type.kind == TypeKind::Double || type.kind == TypeKind::Bool ||
           type.kind == TypeKind::String || type.kind == TypeKind::Any;
}

void SemanticAnalyzer::registerTypeNames() {
    for (const ast::DeclPtr& declaration : program_.declarations()) {
        if (const auto* structure = dynamic_cast<const ast::StructDecl*>(declaration.get())) {
            if (!primitiveType(structure->name()).isError()) {
                report("type name '" + structure->name() + "' conflicts with a built-in type",
                       structure->span());
                continue;
            }
            if (types_.find(structure->name()) != types_.end()) {
                report("duplicate type declaration '" + structure->name() + "'", structure->span());
                continue;
            }
            types_.emplace(structure->name(), TypeSymbol{
                Type{TypeKind::Struct, structure->name()}, structure->span(), {}, {}});
        } else if (const auto* enumeration = dynamic_cast<const ast::EnumDecl*>(declaration.get())) {
            if (!primitiveType(enumeration->name()).isError()) {
                report("type name '" + enumeration->name() + "' conflicts with a built-in type",
                       enumeration->span());
                continue;
            }
            if (types_.find(enumeration->name()) != types_.end()) {
                report("duplicate type declaration '" + enumeration->name() + "'", enumeration->span());
                continue;
            }
            types_.emplace(enumeration->name(), TypeSymbol{
                Type{TypeKind::Enum, enumeration->name()}, enumeration->span(), {}, {}});
        }
    }
}

void SemanticAnalyzer::populateTypeMembers() {
    for (const ast::DeclPtr& declaration : program_.declarations()) {
        if (const auto* structure = dynamic_cast<const ast::StructDecl*>(declaration.get())) {
            auto typeIt = types_.find(structure->name());
            if (typeIt == types_.end() || typeIt->second.type.kind != TypeKind::Struct ||
                typeIt->second.declarationSpan.begin.offset != structure->span().begin.offset) {
                continue;
            }

            for (const ast::FieldDecl& field : structure->fields()) {
                Type fieldType = resolveType(field.type);
                if (fieldType.kind == TypeKind::Void) {
                    report("struct fields cannot have type 'void'", field.span);
                    fieldType = {TypeKind::Error, {}};
                }
                if (typeIt->second.fields.find(field.name) != typeIt->second.fields.end()) {
                    report("duplicate field '" + field.name + "' in struct '" + structure->name() + "'",
                           field.span);
                    continue;
                }
                typeIt->second.fields.emplace(field.name, FieldSymbol{fieldType, &field});
            }
        } else if (const auto* enumeration = dynamic_cast<const ast::EnumDecl*>(declaration.get())) {
            auto typeIt = types_.find(enumeration->name());
            if (typeIt == types_.end() || typeIt->second.type.kind != TypeKind::Enum ||
                typeIt->second.declarationSpan.begin.offset != enumeration->span().begin.offset) {
                continue;
            }

            for (const ast::EnumMember& member : enumeration->members()) {
                if (typeIt->second.enumMembers.find(member.name) != typeIt->second.enumMembers.end()) {
                    report("duplicate enum member '" + member.name + "' in enum '" + enumeration->name() + "'",
                           member.span);
                    continue;
                }
                typeIt->second.enumMembers.emplace(member.name, member.span);
            }
        }
    }
}

void SemanticAnalyzer::registerFunctionSignatures() {
    for (const ast::DeclPtr& declaration : program_.declarations()) {
        const auto* function = dynamic_cast<const ast::FunctionDecl*>(declaration.get());
        if (function == nullptr) continue;

        if (function->name() == "read" || function->name() == "write") {
            report("function name '" + function->name() + "' is reserved for an I/O primitive",
                   function->span());
        }

        const Type returnType = resolveType(function->returnType());
        std::vector<Type> parameterTypes;
        parameterTypes.reserve(function->parameters().size());

        if (function->form() == ast::FunctionForm::SignatureImplementation) {
            const auto existing = functions_.find(function->name());
            if (existing == functions_.end() || !existing->second.hasPrototype) {
                report("signature implementation of '" + function->name() +
                           "' requires a preceding function prototype",
                       function->span());
                parameterTypes.resize(function->parameters().size(), Type{TypeKind::Error, {}});
            } else {
                parameterTypes = existing->second.parameterTypes;
                if (parameterTypes.size() != function->parameters().size()) {
                    report("parameter count in implementation of '" + function->name() +
                               "' does not match its prototype",
                           function->span());
                }
                if (existing->second.returnType != returnType) {
                    report("return type in implementation of '" + function->name() +
                               "' does not match its prototype",
                           function->span());
                }
            }
        } else {
            for (const ast::Parameter& parameter : function->parameters()) {
                if (!parameter.type.has_value()) {
                    report("parameter type is missing", parameter.span);
                    parameterTypes.push_back({TypeKind::Error, {}});
                } else {
                    Type parameterType = resolveType(*parameter.type);
                    if (parameterType.kind == TypeKind::Void) {
                        report("function parameters cannot have type 'void'", parameter.span);
                        parameterType = {TypeKind::Error, {}};
                    }
                    parameterTypes.push_back(std::move(parameterType));
                }
            }
        }

        auto found = functions_.find(function->name());
        if (found == functions_.end()) {
            FunctionSymbol symbol;
            symbol.parameterTypes = parameterTypes;
            symbol.returnType = returnType;
            symbol.hasPrototype = function->form() == ast::FunctionForm::Prototype;
            symbol.hasDefinition = function->form() != ast::FunctionForm::Prototype;
            symbol.declarationSpan = function->span();
            functions_.emplace(function->name(), std::move(symbol));
            continue;
        }

        FunctionSymbol& symbol = found->second;
        if (function->form() == ast::FunctionForm::SignatureImplementation) {
            // The matching and prototype-presence checks were performed above.
            if (symbol.hasDefinition) {
                report("function '" + function->name() + "' has more than one definition", function->span());
            } else {
                symbol.hasDefinition = true;
            }
            continue;
        }

        const bool sameSignature = symbol.parameterTypes == parameterTypes &&
                                   symbol.returnType == returnType;
        if (!sameSignature) {
            report("function overloading is not supported: '" + function->name() +
                       "' was already declared with a different signature",
                   function->span());
            continue;
        }

        if (function->form() == ast::FunctionForm::Prototype) {
            if (symbol.hasPrototype) {
                report("duplicate prototype for function '" + function->name() + "'", function->span());
            } else {
                symbol.hasPrototype = true;
            }
        } else {
            if (symbol.hasDefinition) {
                report("function '" + function->name() + "' has more than one definition", function->span());
            } else {
                symbol.hasDefinition = true;
            }
        }
    }
}

void SemanticAnalyzer::registerGlobalVariables() {
    for (const ast::DeclPtr& declaration : program_.declarations()) {
        const auto* global = dynamic_cast<const ast::GlobalVariableDecl*>(declaration.get());
        if (global == nullptr) continue;

        const ast::VariableSpec& variable = global->variable();
        const Type type = resolveType(variable.type);
        if (type.kind == TypeKind::Void) {
            report("variables cannot have type 'void'", variable.span);
        }
        if (type.kind == TypeKind::Stream && variable.initializer == nullptr) {
            report("stream variables must be initialized", variable.span);
        }
        const auto [it, inserted] = globals_.emplace(
            variable.name, VariableSymbol{type, variable.isMutable, variable.span});
        (void)it;
        if (!inserted) {
            report("duplicate global variable '" + variable.name + "'", variable.span);
        }
    }
}

void SemanticAnalyzer::analyzeTypeDefaults() {
    for (const ast::DeclPtr& declaration : program_.declarations()) {
        const auto* structure = dynamic_cast<const ast::StructDecl*>(declaration.get());
        if (structure == nullptr) continue;

        const auto structureType = types_.find(structure->name());
        if (structureType == types_.end()) continue;

        for (const ast::FieldDecl& field : structure->fields()) {
            if (field.defaultValue == nullptr) continue;
            const FieldSymbol* fieldSymbol = lookupField(structureType->second.type, field.name);
            if (fieldSymbol == nullptr) continue;
            const Type expected = fieldSymbol->type;
            const Type actual = analyzeExpression(*field.defaultValue, &expected);
            if (!canAssign(expected, actual)) {
                report("default value for field '" + field.name + "' has type '" +
                           actual.toString() + "'; expected '" + expected.toString() + "'",
                       field.span);
            }
        }
    }
}

void SemanticAnalyzer::analyzeGlobalInitializers() {
    for (const ast::DeclPtr& declaration : program_.declarations()) {
        const auto* global = dynamic_cast<const ast::GlobalVariableDecl*>(declaration.get());
        if (global == nullptr || global->variable().initializer == nullptr) continue;

        const ast::VariableSpec& variable = global->variable();
        const auto symbol = globals_.find(variable.name);
        if (symbol == globals_.end()) continue;
        const Type expected = symbol->second.type;
        const Type actual = analyzeExpression(*variable.initializer, &expected);
        if (!canAssign(expected, actual)) {
            report("initializer for global variable '" + variable.name + "' has type '" +
                       actual.toString() + "'; expected '" + expected.toString() + "'",
                   variable.span);
        }
    }
}

void SemanticAnalyzer::analyzeFunctionBodies() {
    for (const ast::DeclPtr& declaration : program_.declarations()) {
        const auto* function = dynamic_cast<const ast::FunctionDecl*>(declaration.get());
        if (function != nullptr && function->body() != nullptr) {
            analyzeFunction(*function);
        }
    }
}

void SemanticAnalyzer::analyzeFunction(const ast::FunctionDecl& function) {
    const auto symbolIt = functions_.find(function.name());
    if (symbolIt == functions_.end()) return;

    const FunctionSymbol& functionSymbol = symbolIt->second;
    currentReturnType_ = functionSymbol.returnType;
    insideFunction_ = true;
    loopDepth_ = 0;
    pushScope();

    std::size_t index = 0;
    for (const ast::Parameter& parameter : function.parameters()) {
        if (!parameter.name.has_value()) {
            report("function definition parameter requires a name", parameter.span);
            ++index;
            continue;
        }

        Type parameterType{TypeKind::Error, {}};
        if (index < functionSymbol.parameterTypes.size()) {
            parameterType = functionSymbol.parameterTypes[index];
        } else if (parameter.type.has_value()) {
            parameterType = resolveType(*parameter.type);
        }

        const auto [it, inserted] = scopes_.back().emplace(
            *parameter.name, VariableSymbol{parameterType, false, parameter.span});
        (void)it;
        if (!inserted) {
            report("duplicate parameter name '" + *parameter.name + "'", parameter.span);
        }
        ++index;
    }

    if (function.body() != nullptr) {
        analyzeBlock(*function.body(), false);
    }

    popScope();
    insideFunction_ = false;
    currentReturnType_ = {TypeKind::Error, {}};
}

void SemanticAnalyzer::analyzeBlock(const ast::BlockStmt& block, bool createScope) {
    if (createScope) pushScope();
    for (const ast::StmtPtr& statement : block.statements()) {
        if (statement != nullptr) analyzeStatement(*statement);
    }
    if (createScope) popScope();
}

void SemanticAnalyzer::analyzeStatement(const ast::Statement& statement) {
    if (const auto* block = dynamic_cast<const ast::BlockStmt*>(&statement)) {
        analyzeBlock(*block, true);
    } else if (const auto* variable = dynamic_cast<const ast::VariableDeclStmt*>(&statement)) {
        analyzeVariableDeclaration(variable->variable());
    } else if (const auto* assignment = dynamic_cast<const ast::AssignmentStmt*>(&statement)) {
        analyzeAssignment(*assignment);
    } else if (const auto* expression = dynamic_cast<const ast::ExpressionStmt*>(&statement)) {
        (void)analyzeExpression(expression->expression());
    } else if (const auto* returnStatement = dynamic_cast<const ast::ReturnStmt*>(&statement)) {
        analyzeReturn(*returnStatement);
    } else if (const auto* ifStatement = dynamic_cast<const ast::IfStmt*>(&statement)) {
        analyzeIf(*ifStatement);
    } else if (const auto* whileStatement = dynamic_cast<const ast::WhileStmt*>(&statement)) {
        analyzeWhile(*whileStatement);
    } else if (const auto* forStatement = dynamic_cast<const ast::ForStmt*>(&statement)) {
        analyzeFor(*forStatement);
    } else if (dynamic_cast<const ast::BreakStmt*>(&statement) != nullptr) {
        if (loopDepth_ == 0) report("'break' is only valid inside a loop", statement.span());
    } else if (dynamic_cast<const ast::ContinueStmt*>(&statement) != nullptr) {
        if (loopDepth_ == 0) report("'continue' is only valid inside a loop", statement.span());
    }
}

void SemanticAnalyzer::analyzeVariableDeclaration(const ast::VariableSpec& variable) {
    const Type variableType = resolveType(variable.type);
    if (variableType.kind == TypeKind::Void) {
        report("variables cannot have type 'void'", variable.span);
    }
    if (variableType.kind == TypeKind::Stream && variable.initializer == nullptr) {
        report("stream variables must be initialized", variable.span);
    }
    if (scopes_.empty()) pushScope();

    if (scopes_.back().find(variable.name) != scopes_.back().end()) {
        report("duplicate variable declaration '" + variable.name + "' in this scope", variable.span);
        if (variable.initializer != nullptr) {
            (void)analyzeExpression(*variable.initializer, &variableType);
        }
        return;
    }

    // Serenity variables are default-initialized before an explicit initializer
    // is evaluated. This also makes self-reference deterministic at this stage.
    scopes_.back().emplace(
        variable.name, VariableSymbol{variableType, variable.isMutable, variable.span});

    if (variable.initializer != nullptr) {
        const Type actual = analyzeExpression(*variable.initializer, &variableType);
        if (!canAssign(variableType, actual)) {
            report("initializer for variable '" + variable.name + "' has type '" +
                       actual.toString() + "'; expected '" + variableType.toString() + "'",
                   variable.span);
        }
    }
}

void SemanticAnalyzer::analyzeAssignment(const ast::AssignmentStmt& assignment) {
    const bool directIdentifier = dynamic_cast<const ast::IdentifierExpr*>(&assignment.target()) != nullptr;
    const Type targetType = analyzeLValue(assignment.target(), directIdentifier);
    const Type valueType = analyzeExpression(assignment.value(), &targetType);

    if (assignment.op() == ast::AssignmentOperator::Assign) {
        if (!canAssign(targetType, valueType)) {
            report("cannot assign value of type '" + valueType.toString() +
                       "' to target of type '" + targetType.toString() + "'",
                   assignment.span());
        }
        return;
    }

    if (!isCompoundNumericOperator(assignment.op())) {
        report("unsupported compound assignment operator", assignment.span());
        return;
    }
    if (!targetType.isNumeric()) {
        report("compound arithmetic assignment requires a numeric target", assignment.span());
    } else if (assignment.op() == ast::AssignmentOperator::Modulo && !targetType.isInteger()) {
        report("'%%=' requires an integer target", assignment.span());
    }
    if (!canAssign(targetType, valueType)) {
        report("compound assignment value has type '" + valueType.toString() +
                   "'; expected '" + targetType.toString() + "'",
               assignment.span());
    }
}

void SemanticAnalyzer::analyzeReturn(const ast::ReturnStmt& statement) {
    if (!insideFunction_) {
        report("'return' is only valid inside a function", statement.span());
        (void)analyzeExpression(statement.value());
        return;
    }

    const Type actual = analyzeExpression(statement.value(), &currentReturnType_);
    if (!canAssign(currentReturnType_, actual)) {
        report("return value has type '" + actual.toString() + "'; expected '" +
                   currentReturnType_.toString() + "'",
               statement.span());
    }
}

void SemanticAnalyzer::analyzeIf(const ast::IfStmt& statement) {
    const Type boolType{TypeKind::Bool, {}};
    const Type conditionType = analyzeExpression(statement.condition(), &boolType);
    if (!conditionType.isError() && conditionType.kind != TypeKind::Bool) {
        report("if condition must have type 'bool', not '" + conditionType.toString() + "'",
               statement.condition().span());
    }

    analyzeBlock(statement.thenBranch(), true);
    if (statement.elseBranch() != nullptr) {
        analyzeStatement(*statement.elseBranch());
    }
}

void SemanticAnalyzer::analyzeWhile(const ast::WhileStmt& statement) {
    const Type boolType{TypeKind::Bool, {}};
    const Type conditionType = analyzeExpression(statement.condition(), &boolType);
    if (!conditionType.isError() && conditionType.kind != TypeKind::Bool) {
        report("while condition must have type 'bool', not '" + conditionType.toString() + "'",
               statement.condition().span());
    }

    ++loopDepth_;
    analyzeBlock(statement.body(), true);
    --loopDepth_;
}

void SemanticAnalyzer::analyzeFor(const ast::ForStmt& statement) {
    // The initializer's declaration belongs to the loop scope and remains
    // visible to the condition, body, and increment, but not after the loop.
    pushScope();
    if (statement.initializer() != nullptr) {
        analyzeStatement(*statement.initializer());
    }

    if (statement.condition() != nullptr) {
        const Type boolType{TypeKind::Bool, {}};
        const Type conditionType = analyzeExpression(*statement.condition(), &boolType);
        if (!conditionType.isError() && conditionType.kind != TypeKind::Bool) {
            report("for condition must have type 'bool', not '" + conditionType.toString() + "'",
                   statement.condition()->span());
        }
    }

    ++loopDepth_;
    analyzeBlock(statement.body(), true);
    if (statement.increment() != nullptr) {
        analyzeStatement(*statement.increment());
    }
    --loopDepth_;
    popScope();
}

Type SemanticAnalyzer::analyzeExpression(
    const ast::Expression& expression,
    const Type* expected) {
    Type result{TypeKind::Error, {}};

    if (dynamic_cast<const ast::IntegerLiteralExpr*>(&expression) != nullptr) {
        result = {TypeKind::Int, {}};
    } else if (dynamic_cast<const ast::FloatingLiteralExpr*>(&expression) != nullptr) {
        if (expected != nullptr &&
            (expected->kind == TypeKind::Float || expected->kind == TypeKind::Double)) {
            result = *expected;
        } else {
            // An unconstrained floating literal defaults to double. In a typed
            // initializer, argument, or return context it adopts float/double.
            result = {TypeKind::Double, {}};
        }
    } else if (dynamic_cast<const ast::StringLiteralExpr*>(&expression) != nullptr) {
        result = {TypeKind::String, {}};
    } else if (dynamic_cast<const ast::BooleanLiteralExpr*>(&expression) != nullptr) {
        result = {TypeKind::Bool, {}};
    } else if (dynamic_cast<const ast::NullLiteralExpr*>(&expression) != nullptr) {
        result = {TypeKind::Null, {}};
    } else if (const auto* identifier = dynamic_cast<const ast::IdentifierExpr*>(&expression)) {
        const VariableSymbol* variable = lookupVariable(identifier->name());
        if (variable != nullptr) {
            result = variable->type;
        } else if (identifier->name() == "stdout" || identifier->name() == "stderr" ||
                   identifier->name() == "stdin") {
            result = {TypeKind::Stream, {}};
        } else {
            report("unknown variable '" + identifier->name() + "'", expression.span());
        }
    } else if (const auto* unary = dynamic_cast<const ast::UnaryExpr*>(&expression)) {
        const Type operand = analyzeExpression(unary->operand(), expected);
        switch (unary->op()) {
        case ast::UnaryOperator::Plus:
        case ast::UnaryOperator::Minus:
            if (!operand.isError() && !operand.isNumeric()) {
                report("unary '+' and '-' require a numeric operand", expression.span());
            } else {
                result = operand;
            }
            break;
        case ast::UnaryOperator::LogicalNot:
            if (!operand.isError() && operand.kind != TypeKind::Bool) {
                report("logical '!' requires a bool operand", expression.span());
            } else if (!operand.isError()) {
                result = {TypeKind::Bool, {}};
            }
            break;
        case ast::UnaryOperator::BitwiseNot:
            if (!operand.isError() && !operand.isInteger()) {
                report("bitwise '~' requires an int operand", expression.span());
            } else if (!operand.isError()) {
                result = {TypeKind::Int, {}};
            }
            break;
        }
    } else if (const auto* binary = dynamic_cast<const ast::BinaryExpr*>(&expression)) {
        result = analyzeBinaryExpression(*binary, expected);
    } else if (const auto* member = dynamic_cast<const ast::MemberAccessExpr*>(&expression)) {
        result = analyzeMemberAccess(*member);
    } else if (const auto* call = dynamic_cast<const ast::CallExpr*>(&expression)) {
        result = analyzeCallExpression(*call, expected);
    } else if (const auto* initializer = dynamic_cast<const ast::StructInitializerExpr*>(&expression)) {
        result = analyzeStructInitializer(*initializer, expected);
    }

    result_.model.expressionTypes.insert_or_assign(&expression, result);
    return result;
}

Type SemanticAnalyzer::analyzeBinaryExpression(
    const ast::BinaryExpr& expression,
    const Type* expected) {
    const ast::BinaryOperator op = expression.op();
    const bool numericOperation = isNumericOperator(op);
    const Type* operandExpected = nullptr;
    if (numericOperation && expected != nullptr && expected->isNumeric()) {
        operandExpected = expected;
    }

    Type left{TypeKind::Error, {}};
    Type right{TypeKind::Error, {}};

    if (operandExpected != nullptr) {
        left = analyzeExpression(expression.left(), operandExpected);
        right = analyzeExpression(expression.right(), &left);
    } else if (isFloatingLiteral(expression.left()) &&
               !isFloatingLiteral(expression.right())) {
        right = analyzeExpression(expression.right());
        left = analyzeExpression(expression.left(), &right);
    } else if (dynamic_cast<const ast::NullLiteralExpr*>(&expression.left()) != nullptr) {
        right = analyzeExpression(expression.right());
        left = analyzeExpression(expression.left(), &right);
    } else {
        left = analyzeExpression(expression.left());
        right = analyzeExpression(expression.right(), &left);
    }

    if (left.isError() || right.isError()) return {TypeKind::Error, {}};

    switch (op) {
    case ast::BinaryOperator::Add:
    case ast::BinaryOperator::Subtract:
    case ast::BinaryOperator::Multiply:
    case ast::BinaryOperator::Divide:
        if (!left.isNumeric() || left != right) {
            report("arithmetic operands must have the same numeric type; received '" +
                       left.toString() + "' and '" + right.toString() + "'",
                   expression.span());
            return {TypeKind::Error, {}};
        }
        return left;

    case ast::BinaryOperator::Modulo:
        if (!left.isInteger() || !right.isInteger()) {
            report("'%%' requires two int operands", expression.span());
            return {TypeKind::Error, {}};
        }
        return {TypeKind::Int, {}};

    case ast::BinaryOperator::Equal:
    case ast::BinaryOperator::NotEqual:
        if (left == right ||
            (left.kind == TypeKind::Null && right.isNullable()) ||
            (right.kind == TypeKind::Null && left.isNullable())) {
            return {TypeKind::Bool, {}};
        }
        report("equality operands must have compatible types; received '" +
                   left.toString() + "' and '" + right.toString() + "'",
               expression.span());
        return {TypeKind::Error, {}};

    case ast::BinaryOperator::Less:
    case ast::BinaryOperator::Greater:
    case ast::BinaryOperator::LessEqual:
    case ast::BinaryOperator::GreaterEqual:
        if (!left.isNumeric() || left != right) {
            report("relational operands must have the same numeric type; received '" +
                       left.toString() + "' and '" + right.toString() + "'",
                   expression.span());
            return {TypeKind::Error, {}};
        }
        return {TypeKind::Bool, {}};

    case ast::BinaryOperator::LogicalAnd:
    case ast::BinaryOperator::LogicalOr:
        if (left.kind != TypeKind::Bool || right.kind != TypeKind::Bool) {
            report("logical operators require two bool operands", expression.span());
            return {TypeKind::Error, {}};
        }
        return {TypeKind::Bool, {}};

    case ast::BinaryOperator::BitwiseAnd:
    case ast::BinaryOperator::BitwiseOr:
    case ast::BinaryOperator::BitwiseXor:
    case ast::BinaryOperator::ShiftLeft:
    case ast::BinaryOperator::ShiftRight:
        if (!left.isInteger() || !right.isInteger()) {
            report("bitwise and shift operators require two int operands", expression.span());
            return {TypeKind::Error, {}};
        }
        return {TypeKind::Int, {}};
    }

    return {TypeKind::Error, {}};
}

Type SemanticAnalyzer::analyzeCallExpression(
    const ast::CallExpr& expression,
    const Type* expected) {
    (void)expected;

    if (expression.callee() == "write") {
        if (expression.arguments().size() != 2) {
            report("built-in 'write' expects a stream and a printable value", expression.span());
        }
        if (!expression.arguments().empty() && expression.arguments()[0] != nullptr) {
            const Type streamType = analyzeExpression(*expression.arguments()[0]);
            if (!streamType.isError() && streamType.kind != TypeKind::Stream) {
                report("first argument to 'write' must be a stream", expression.arguments()[0]->span());
            }
        }
        if (expression.arguments().size() >= 2 && expression.arguments()[1] != nullptr) {
            const Type valueType = analyzeExpression(*expression.arguments()[1]);
            if (!valueType.isError() && !isPrintable(valueType)) {
                report("second argument to 'write' must be a primitive printable value",
                       expression.arguments()[1]->span());
            }
        }
        for (std::size_t index = 2; index < expression.arguments().size(); ++index) {
            if (expression.arguments()[index] != nullptr) (void)analyzeExpression(*expression.arguments()[index]);
        }
        return {TypeKind::Void, {}};
    }

    if (expression.callee() == "read") {
        if (expression.arguments().size() != 1) {
            report("built-in 'read' expects exactly one stream argument", expression.span());
        }
        for (std::size_t index = 0; index < expression.arguments().size(); ++index) {
            if (expression.arguments()[index] == nullptr) continue;
            const Type streamType = analyzeExpression(*expression.arguments()[index]);
            if (!streamType.isError() && streamType.kind != TypeKind::Stream) {
                report("argument to 'read' must be a stream", expression.arguments()[index]->span());
            }
        }
        return {TypeKind::String, {}};
    }

    const auto function = functions_.find(expression.callee());
    if (function == functions_.end()) {
        // Keep the original output intrinsics available for source files that
        // have not included std/io.snh yet. The standard-library definitions
        // take precedence whenever they are present in the program.
        if (expression.callee() == "print" || expression.callee() == "println") {
            if (expression.arguments().size() != 1) {
                report("built-in '" + expression.callee() + "' currently accepts exactly one argument",
                       expression.span());
            }
            for (const ast::ExprPtr& argument : expression.arguments()) {
                if (argument == nullptr) continue;
                const Type argumentType = analyzeExpression(*argument);
                if (!argumentType.isError() && !isPrintable(argumentType)) {
                    report("built-in '" + expression.callee() + "' cannot print values of type '" +
                               argumentType.toString() + "'", argument->span());
                }
            }
            return {TypeKind::Void, {}};
        }
        report("unknown function '" + expression.callee() + "'", expression.span());
        for (const ast::ExprPtr& argument : expression.arguments()) {
            if (argument != nullptr) (void)analyzeExpression(*argument);
        }
        return {TypeKind::Error, {}};
    }

    if (expression.arguments().size() != function->second.parameterTypes.size()) {
        report("function '" + expression.callee() + "' expects " +
                   std::to_string(function->second.parameterTypes.size()) + " argument(s), but received " +
                   std::to_string(expression.arguments().size()),
               expression.span());
    }

    const std::size_t count = std::min(
        expression.arguments().size(), function->second.parameterTypes.size());
    for (std::size_t index = 0; index < expression.arguments().size(); ++index) {
        if (expression.arguments()[index] == nullptr) continue;
        if (index < count) {
            const Type& expectedType = function->second.parameterTypes[index];
            const Type actual = analyzeExpression(*expression.arguments()[index], &expectedType);
            if (!canAssign(expectedType, actual)) {
                report("argument " + std::to_string(index + 1) + " of '" + expression.callee() +
                           "' has type '" + actual.toString() + "'; expected '" +
                           expectedType.toString() + "'",
                       expression.arguments()[index]->span());
            }
        } else {
            (void)analyzeExpression(*expression.arguments()[index]);
        }
    }

    return function->second.returnType;
}

Type SemanticAnalyzer::analyzeMemberAccess(const ast::MemberAccessExpr& expression) {
    if (const auto* identifier = dynamic_cast<const ast::IdentifierExpr*>(&expression.object())) {
        const auto type = types_.find(identifier->name());
        if (type != types_.end() && type->second.type.kind == TypeKind::Enum) {
            if (type->second.enumMembers.find(expression.member()) == type->second.enumMembers.end()) {
                report("enum '" + identifier->name() + "' has no member '" + expression.member() + "'",
                       expression.span());
                return {TypeKind::Error, {}};
            }
            return type->second.type;
        }
    }

    const Type objectType = analyzeExpression(expression.object());
    if (objectType.isError()) return objectType;
    if (objectType.kind != TypeKind::Struct) {
        report("member access requires a struct value; received '" + objectType.toString() + "'",
               expression.span());
        return {TypeKind::Error, {}};
    }

    const FieldSymbol* field = lookupField(objectType, expression.member());
    if (field == nullptr) {
        report("struct '" + objectType.toString() + "' has no field '" + expression.member() + "'",
               expression.span());
        return {TypeKind::Error, {}};
    }
    return field->type;
}

Type SemanticAnalyzer::analyzeStructInitializer(
    const ast::StructInitializerExpr& expression,
    const Type* expected) {
    if (expected == nullptr || expected->kind != TypeKind::Struct) {
        report("struct initializer requires a declared struct type", expression.span());
        for (const ast::DesignatedInitializer& field : expression.fields()) {
            if (field.value != nullptr) (void)analyzeExpression(*field.value);
        }
        return {TypeKind::Error, {}};
    }

    const auto type = types_.find(expected->name);
    if (type == types_.end()) return {TypeKind::Error, {}};

    std::unordered_map<std::string, bool> seenFields;
    for (const ast::DesignatedInitializer& initializer : expression.fields()) {
        if (!seenFields.emplace(initializer.fieldName, true).second) {
            report("field '" + initializer.fieldName + "' is initialized more than once",
                   initializer.span);
            if (initializer.value != nullptr) (void)analyzeExpression(*initializer.value);
            continue;
        }

        const FieldSymbol* field = lookupField(*expected, initializer.fieldName);
        if (field == nullptr) {
            report("struct '" + expected->toString() + "' has no field '" +
                       initializer.fieldName + "'",
                   initializer.span);
            if (initializer.value != nullptr) (void)analyzeExpression(*initializer.value);
            continue;
        }

        if (initializer.value != nullptr) {
            const Type actual = analyzeExpression(*initializer.value, &field->type);
            if (!canAssign(field->type, actual)) {
                report("initializer for field '" + initializer.fieldName + "' has type '" +
                           actual.toString() + "'; expected '" + field->type.toString() + "'",
                       initializer.span);
            }
        }
    }

    // Omitted fields receive the language default for their declared type.
    // Those defaults are validated as part of the type declaration itself.
    return *expected;
}

Type SemanticAnalyzer::analyzeLValue(const ast::Expression& expression, bool directAssignment) {
    if (const auto* identifier = dynamic_cast<const ast::IdentifierExpr*>(&expression)) {
        VariableSymbol* variable = lookupVariable(identifier->name());
        if (variable == nullptr) {
            report("unknown variable '" + identifier->name() + "'", expression.span());
            return {TypeKind::Error, {}};
        }
        if (directAssignment && !variable->isMutable) {
            report("cannot assign to immutable variable '" + identifier->name() + "'",
                   expression.span());
        }
        return variable->type;
    }

    if (const auto* member = dynamic_cast<const ast::MemberAccessExpr*>(&expression)) {
        if (const auto* qualifier = dynamic_cast<const ast::IdentifierExpr*>(&member->object())) {
            const auto type = types_.find(qualifier->name());
            if (type != types_.end() && type->second.type.kind == TypeKind::Enum) {
                report("enum members are values and cannot be assigned to", expression.span());
                return {TypeKind::Error, {}};
            }
        }
        // Struct bindings are immutable by default. Mutating any nested field
        // requires the root variable holding the struct to have been declared
        // with `mut`.
        const ast::Expression* root = &member->object();
        while (const auto* nested = dynamic_cast<const ast::MemberAccessExpr*>(root)) {
            root = &nested->object();
        }
        if (const auto* rootIdentifier = dynamic_cast<const ast::IdentifierExpr*>(root)) {
            const VariableSymbol* variable = lookupVariable(rootIdentifier->name());
            if (variable != nullptr && !variable->isMutable) {
                report("cannot modify fields of immutable variable '" + rootIdentifier->name() + "'",
                       expression.span());
            }
        } else {
            report("cannot modify a field through a temporary struct value", expression.span());
        }
        return analyzeMemberAccess(*member);
    }

    report("expression is not assignable", expression.span());
    return {TypeKind::Error, {}};
}

SemanticAnalyzer::VariableSymbol* SemanticAnalyzer::lookupVariable(const std::string& name) {
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
        const auto found = scope->find(name);
        if (found != scope->end()) return &found->second;
    }
    const auto global = globals_.find(name);
    return global == globals_.end() ? nullptr : &global->second;
}

const SemanticAnalyzer::SemanticAnalyzer::VariableSymbol* SemanticAnalyzer::lookupVariable(
    const std::string& name) const {
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
        const auto found = scope->find(name);
        if (found != scope->end()) return &found->second;
    }
    const auto global = globals_.find(name);
    return global == globals_.end() ? nullptr : &global->second;
}

const SemanticAnalyzer::FieldSymbol* SemanticAnalyzer::lookupField(
    const Type& type,
    const std::string& name) const {
    const auto typeIt = types_.find(type.name);
    if (typeIt == types_.end() || typeIt->second.type.kind != TypeKind::Struct) return nullptr;
    const auto field = typeIt->second.fields.find(name);
    return field == typeIt->second.fields.end() ? nullptr : &field->second;
}

bool SemanticAnalyzer::isFloatingLiteral(const ast::Expression& expression) const noexcept {
    return dynamic_cast<const ast::FloatingLiteralExpr*>(&expression) != nullptr;
}

bool SemanticAnalyzer::isNumericOperator(ast::BinaryOperator op) const noexcept {
    return op == ast::BinaryOperator::Add || op == ast::BinaryOperator::Subtract ||
           op == ast::BinaryOperator::Multiply || op == ast::BinaryOperator::Divide ||
           op == ast::BinaryOperator::Modulo || op == ast::BinaryOperator::Less ||
           op == ast::BinaryOperator::Greater || op == ast::BinaryOperator::LessEqual ||
           op == ast::BinaryOperator::GreaterEqual;
}

bool SemanticAnalyzer::isCompoundNumericOperator(ast::AssignmentOperator op) const noexcept {
    return op == ast::AssignmentOperator::Add || op == ast::AssignmentOperator::Subtract ||
           op == ast::AssignmentOperator::Multiply || op == ast::AssignmentOperator::Divide ||
           op == ast::AssignmentOperator::Modulo;
}

void SemanticAnalyzer::pushScope() {
    scopes_.emplace_back();
}

void SemanticAnalyzer::popScope() noexcept {
    if (!scopes_.empty()) scopes_.pop_back();
}

void SemanticAnalyzer::report(std::string message, ast::SourceSpan span) {
    report(std::move(message), span.begin);
}

void SemanticAnalyzer::report(std::string message, SourceLocation location) {
    result_.errors.push_back(SemanticError{std::move(message), location});
}

} // namespace serenity::semantic
