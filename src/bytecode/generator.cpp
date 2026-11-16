#include "serenity/bytecode/generator.hpp"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace serenity::bytecode {
namespace {

using TypeKind = semantic::TypeKind;

[[nodiscard]] TypeInfo toTypeInfo(const semantic::Type& type) {
    switch (type.kind) {
    case TypeKind::Void: return {ValueType::Void, {}};
    case TypeKind::Null: return {ValueType::Null, {}};
    case TypeKind::Int: return {ValueType::Int, {}};
    case TypeKind::Float: return {ValueType::Float, {}};
    case TypeKind::Double: return {ValueType::Double, {}};
    case TypeKind::Bool: return {ValueType::Bool, {}};
    case TypeKind::String: return {ValueType::String, {}};
    case TypeKind::Struct: return {ValueType::Struct, type.name};
    case TypeKind::Enum: return {ValueType::Enum, type.name};
    case TypeKind::Any: return {ValueType::Any, {}};
    case TypeKind::Stream: return {ValueType::Stream, {}};
    case TypeKind::Error: return {ValueType::Invalid, {}};
    }
    return {ValueType::Invalid, {}};
}

[[nodiscard]] bool fitsInstructionOperand(std::size_t value) noexcept {
    return value <= static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max());
}

[[nodiscard]] std::optional<std::int64_t> parseInteger(std::string_view spelling) {
    std::string owned(spelling);
    char* end = nullptr;
    errno = 0;
    const long long parsed = std::strtoll(owned.c_str(), &end, 10);
    if (errno == ERANGE || end == owned.c_str() || end == nullptr || *end != '\0') {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(parsed);
}

[[nodiscard]] std::optional<float> parseFloat(std::string_view spelling) {
    std::string owned(spelling);
    char* end = nullptr;
    errno = 0;
    const float parsed = std::strtof(owned.c_str(), &end);
    if (errno == ERANGE || end == owned.c_str() || end == nullptr || *end != '\0' ||
        !std::isfinite(parsed)) {
        return std::nullopt;
    }
    return parsed;
}

[[nodiscard]] std::optional<double> parseDouble(std::string_view spelling) {
    std::string owned(spelling);
    char* end = nullptr;
    errno = 0;
    const double parsed = std::strtod(owned.c_str(), &end);
    if (errno == ERANGE || end == owned.c_str() || end == nullptr || *end != '\0' ||
        !std::isfinite(parsed)) {
        return std::nullopt;
    }
    return parsed;
}

class ModuleBuilder;

class FunctionCompiler {
public:
    FunctionCompiler(
        ModuleBuilder& owner,
        Function& function,
        std::unordered_map<std::string, std::uint32_t>& functionIndices,
        const std::unordered_map<std::string, std::uint32_t>& globalIndices,
        const std::unordered_map<std::string, std::uint32_t>& structIndices,
        const std::unordered_map<std::string, std::uint32_t>& enumIndices,
        const std::unordered_map<std::string, std::unordered_map<std::string, std::uint32_t>>& fieldIndices,
        bool synthetic = false);

    void compileFunction(const ast::FunctionDecl& declaration);
    void compileGlobalInitializers(const std::vector<const ast::GlobalVariableDecl*>& globals);

private:
    struct Binding {
        bool global {false};
        std::uint32_t index {0};
        TypeInfo type;
        bool isMutable {false};
    };

    struct LoopContext {
        std::size_t continueTarget {0};
        std::vector<std::size_t> breakJumps;
    };

    ModuleBuilder& owner_;
    Function& function_;
    std::unordered_map<std::string, std::uint32_t>& functionIndices_;
    const std::unordered_map<std::string, std::uint32_t>& globalIndices_;
    const std::unordered_map<std::string, std::uint32_t>& structIndices_;
    const std::unordered_map<std::string, std::uint32_t>& enumIndices_;
    const std::unordered_map<std::string, std::unordered_map<std::string, std::uint32_t>>& fieldIndices_;
    std::vector<std::unordered_map<std::string, Binding>> scopes_;
    std::vector<LoopContext> loops_;
    mutable Binding globalBindingScratch_;

    void report(std::string message, ast::SourceSpan span);
    [[nodiscard]] std::size_t emit(
        Opcode opcode,
        ValueType type,
        std::int32_t operand,
        std::int32_t extra,
        ast::SourceSpan span);
    [[nodiscard]] std::size_t emit(Opcode opcode, ast::SourceSpan span = {});
    void patchJump(std::size_t instruction, std::size_t target, ast::SourceSpan span);

    [[nodiscard]] TypeInfo typeInfo(const ast::TypeRef& type, ast::SourceSpan span);
    [[nodiscard]] semantic::Type semanticType(const ast::Expression& expression) const;
    [[nodiscard]] TypeInfo expressionType(const ast::Expression& expression) const;
    [[nodiscard]] Binding* lookup(const std::string& name);
    [[nodiscard]] std::optional<std::uint32_t> allocateLocal(
        const std::string& name, TypeInfo type, bool isMutable, bool isParameter,
        ast::SourceSpan span);

    void emitConstant(Constant constant, ast::SourceSpan span);
    void emitDefault(TypeInfo type, ast::SourceSpan span);

    void compileBlock(const ast::BlockStmt& block, bool createScope);
    void compileStatement(const ast::Statement& statement);
    void compileVariable(const ast::VariableSpec& variable, ast::SourceSpan span);
    void compileAssignment(const ast::AssignmentStmt& assignment);
    void compileReturn(const ast::ReturnStmt& statement);
    void compileIf(const ast::IfStmt& statement);
    void compileWhile(const ast::WhileStmt& statement);
    void compileExpression(const ast::Expression& expression);
    void compileCall(const ast::CallExpr& expression);
    void compileMemberAccess(const ast::MemberAccessExpr& expression);
    void compileStructInitializer(const ast::StructInitializerExpr& expression);

    void load(const Binding& binding, ast::SourceSpan span);
    void store(const Binding& binding, ast::SourceSpan span);
    void compileCompoundOperator(ast::AssignmentOperator op, TypeInfo type, ast::SourceSpan span);
    [[nodiscard]] Opcode binaryOpcode(ast::BinaryOperator op) const;
};

class ModuleBuilder {
public:
    ModuleBuilder(const ast::Program& program, const semantic::SemanticModel& model)
        : program_(program), model_(model) {}

    BytecodeResult build() {
        registerTypes();
        registerGlobals();
        registerFunctions();
        if (!errors_.empty()) return {std::nullopt, std::move(errors_)};

        compileFunctions();
        compileGlobalInitializer();
        if (!errors_.empty()) return {std::nullopt, std::move(errors_)};

        return {std::move(module_), {}};
    }

    [[nodiscard]] const semantic::SemanticModel& semanticModel() const noexcept { return model_; }
    [[nodiscard]] BytecodeModule& module() noexcept { return module_; }
    [[nodiscard]] const std::unordered_map<std::string, std::uint32_t>& functionIndices() const noexcept {
        return functionIndices_;
    }
    [[nodiscard]] const std::unordered_map<std::string, std::uint32_t>& globalIndices() const noexcept {
        return globalIndices_;
    }
    [[nodiscard]] const std::unordered_map<std::string, std::uint32_t>& structIndices() const noexcept {
        return structIndices_;
    }
    [[nodiscard]] const std::unordered_map<std::string, std::uint32_t>& enumIndices() const noexcept {
        return enumIndices_;
    }
    [[nodiscard]] const std::unordered_map<std::string, std::unordered_map<std::string, std::uint32_t>>&
    fieldIndices() const noexcept { return fieldIndices_; }

    void report(std::string message, ast::SourceSpan span) {
        errors_.push_back(BytecodeError{std::move(message), span.begin});
    }

    [[nodiscard]] std::uint32_t addConstant(Constant constant, ast::SourceSpan span) {
        if (module_.constants.size() >= static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
            report("bytecode constant pool exceeds the supported index range", span);
            return 0;
        }
        const auto index = static_cast<std::uint32_t>(module_.constants.size());
        module_.constants.push_back(std::move(constant));
        return index;
    }

    [[nodiscard]] std::optional<Constant> constantExpression(const ast::Expression& expression) const {
        const auto semanticIt = model_.expressionTypes.find(&expression);
        if (semanticIt == model_.expressionTypes.end()) return std::nullopt;
        const TypeInfo type = toTypeInfo(semanticIt->second);

        if (const auto* integer = dynamic_cast<const ast::IntegerLiteralExpr*>(&expression)) {
            const auto value = parseInteger(integer->spelling());
            if (!value) return std::nullopt;
            return Constant{ValueType::Int, *value};
        }
        if (const auto* floating = dynamic_cast<const ast::FloatingLiteralExpr*>(&expression)) {
            if (type.kind == ValueType::Float) {
                const auto value = parseFloat(floating->spelling());
                if (!value) return std::nullopt;
                return Constant{ValueType::Float, *value};
            }
            const auto value = parseDouble(floating->spelling());
            if (!value) return std::nullopt;
            return Constant{ValueType::Double, *value};
        }
        if (const auto* string = dynamic_cast<const ast::StringLiteralExpr*>(&expression)) {
            return Constant{ValueType::String, string->value()};
        }
        if (const auto* boolean = dynamic_cast<const ast::BooleanLiteralExpr*>(&expression)) {
            return Constant{ValueType::Bool, boolean->value()};
        }
        if (dynamic_cast<const ast::NullLiteralExpr*>(&expression) != nullptr) {
            return Constant{ValueType::Null, std::monostate{}};
        }
        if (const auto* member = dynamic_cast<const ast::MemberAccessExpr*>(&expression)) {
            if (const auto* qualifier = dynamic_cast<const ast::IdentifierExpr*>(&member->object())) {
                const auto enumIndex = enumIndices_.find(qualifier->name());
                if (enumIndex != enumIndices_.end()) {
                    for (const ast::DeclPtr& declaration : program_.declarations()) {
                        const auto* enumeration = dynamic_cast<const ast::EnumDecl*>(declaration.get());
                        if (enumeration == nullptr || enumeration->name() != qualifier->name()) continue;
                        for (std::size_t index = 0; index < enumeration->members().size(); ++index) {
                            if (enumeration->members()[index].name == member->member()) {
                                return Constant{
                                    ValueType::Enum,
                                    EnumConstantValue{enumIndex->second, static_cast<std::uint32_t>(index)}};
                            }
                        }
                    }
                }
            }
        }
        if (const auto* unary = dynamic_cast<const ast::UnaryExpr*>(&expression)) {
            auto operand = constantExpression(unary->operand());
            if (!operand) return std::nullopt;
            switch (unary->op()) {
            case ast::UnaryOperator::Plus:
                return operand;
            case ast::UnaryOperator::Minus:
                if (auto* integerValue = std::get_if<std::int64_t>(&operand->value)) {
                    if (*integerValue == std::numeric_limits<std::int64_t>::min()) return std::nullopt;
                    *integerValue = -*integerValue;
                } else if (auto* floatValue = std::get_if<float>(&operand->value)) *floatValue = -*floatValue;
                else if (auto* doubleValue = std::get_if<double>(&operand->value)) *doubleValue = -*doubleValue;
                else return std::nullopt;
                return operand;
            case ast::UnaryOperator::LogicalNot:
                if (auto* value = std::get_if<bool>(&operand->value)) {
                    return Constant{ValueType::Bool, !*value};
                }
                return std::nullopt;
            case ast::UnaryOperator::BitwiseNot:
                if (auto* value = std::get_if<std::int64_t>(&operand->value)) {
                    return Constant{ValueType::Int, static_cast<std::int64_t>(~*value)};
                }
                return std::nullopt;
            }
        }
        if (const auto* binary = dynamic_cast<const ast::BinaryExpr*>(&expression)) {
            auto left = constantExpression(binary->left());
            auto right = constantExpression(binary->right());
            if (!left || !right) return std::nullopt;
            return evaluateBinaryConstant(*binary, std::move(*left), std::move(*right));
        }
        return std::nullopt;
    }

private:
    friend class FunctionCompiler;

    const ast::Program& program_;
    const semantic::SemanticModel& model_;
    BytecodeModule module_;
    std::vector<BytecodeError> errors_;
    std::unordered_map<std::string, std::uint32_t> structIndices_;
    std::unordered_map<std::string, std::uint32_t> enumIndices_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::uint32_t>> fieldIndices_;
    std::unordered_map<std::string, std::uint32_t> globalIndices_;
    std::unordered_map<std::string, std::uint32_t> functionIndices_;
    std::unordered_map<std::string, std::vector<ast::TypeRef>> prototypeParameterTypes_;
    std::vector<const ast::GlobalVariableDecl*> globalDeclarations_;
    std::vector<const ast::FunctionDecl*> functionDefinitions_;

    [[nodiscard]] static std::optional<Constant> evaluateBinaryConstant(
        const ast::BinaryExpr& expression, Constant left, Constant right) {
        const auto intLeft = std::get_if<std::int64_t>(&left.value);
        const auto intRight = std::get_if<std::int64_t>(&right.value);
        if (intLeft != nullptr && intRight != nullptr) {
            const std::int64_t a = *intLeft;
            const std::int64_t b = *intRight;
            switch (expression.op()) {
            case ast::BinaryOperator::Add: {
                if ((b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) ||
                    (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b)) return std::nullopt;
                return Constant{ValueType::Int, a + b};
            }
            case ast::BinaryOperator::Subtract: {
                if ((b < 0 && a > std::numeric_limits<std::int64_t>::max() + b) ||
                    (b > 0 && a < std::numeric_limits<std::int64_t>::min() + b)) return std::nullopt;
                return Constant{ValueType::Int, a - b};
            }
            case ast::BinaryOperator::Multiply: {
                if (a == 0 || b == 0) return Constant{ValueType::Int, std::int64_t{0}};
                if ((a == -1 && b == std::numeric_limits<std::int64_t>::min()) ||
                    (b == -1 && a == std::numeric_limits<std::int64_t>::min())) return std::nullopt;
                if (a > 0) {
                    if ((b > 0 && a > std::numeric_limits<std::int64_t>::max() / b) ||
                        (b < 0 && b < std::numeric_limits<std::int64_t>::min() / a)) return std::nullopt;
                } else {
                    if ((b > 0 && a < std::numeric_limits<std::int64_t>::min() / b) ||
                        (b < 0 && a < std::numeric_limits<std::int64_t>::max() / b)) return std::nullopt;
                }
                return Constant{ValueType::Int, a * b};
            }
            case ast::BinaryOperator::Divide:
                if (b != 0 && !(a == std::numeric_limits<std::int64_t>::min() && b == -1))
                    return Constant{ValueType::Int, a / b};
                return std::nullopt;
            case ast::BinaryOperator::Modulo:
                if (b != 0 && !(a == std::numeric_limits<std::int64_t>::min() && b == -1))
                    return Constant{ValueType::Int, a % b};
                return std::nullopt;
            case ast::BinaryOperator::Equal: return Constant{ValueType::Bool, a == b};
            case ast::BinaryOperator::NotEqual: return Constant{ValueType::Bool, a != b};
            case ast::BinaryOperator::Less: return Constant{ValueType::Bool, a < b};
            case ast::BinaryOperator::LessEqual: return Constant{ValueType::Bool, a <= b};
            case ast::BinaryOperator::Greater: return Constant{ValueType::Bool, a > b};
            case ast::BinaryOperator::GreaterEqual: return Constant{ValueType::Bool, a >= b};
            case ast::BinaryOperator::BitwiseAnd: return Constant{ValueType::Int, a & b};
            case ast::BinaryOperator::BitwiseOr: return Constant{ValueType::Int, a | b};
            case ast::BinaryOperator::BitwiseXor: return Constant{ValueType::Int, a ^ b};
            case ast::BinaryOperator::ShiftLeft:
                if (b < 0 || b >= 63 || a < 0 || a > (std::numeric_limits<std::int64_t>::max() >> b))
                    return std::nullopt;
                return Constant{ValueType::Int, a << b};
            case ast::BinaryOperator::ShiftRight:
                if (b < 0 || b >= 63) return std::nullopt;
                return Constant{ValueType::Int, a >> b};
            case ast::BinaryOperator::LogicalAnd: return std::nullopt;
            case ast::BinaryOperator::LogicalOr: return std::nullopt;
            }
        }

        if (const auto* a = std::get_if<float>(&left.value)) {
            if (const auto* b = std::get_if<float>(&right.value)) {
                switch (expression.op()) {
                case ast::BinaryOperator::Add: return Constant{ValueType::Float, *a + *b};
                case ast::BinaryOperator::Subtract: return Constant{ValueType::Float, *a - *b};
                case ast::BinaryOperator::Multiply: return Constant{ValueType::Float, *a * *b};
                case ast::BinaryOperator::Divide: if (*b != 0.0F) return Constant{ValueType::Float, *a / *b}; return std::nullopt;
                case ast::BinaryOperator::Equal: return Constant{ValueType::Bool, *a == *b};
                case ast::BinaryOperator::NotEqual: return Constant{ValueType::Bool, *a != *b};
                case ast::BinaryOperator::Less: return Constant{ValueType::Bool, *a < *b};
                case ast::BinaryOperator::LessEqual: return Constant{ValueType::Bool, *a <= *b};
                case ast::BinaryOperator::Greater: return Constant{ValueType::Bool, *a > *b};
                case ast::BinaryOperator::GreaterEqual: return Constant{ValueType::Bool, *a >= *b};
                default: return std::nullopt;
                }
            }
        }
        if (const auto* a = std::get_if<double>(&left.value)) {
            if (const auto* b = std::get_if<double>(&right.value)) {
                switch (expression.op()) {
                case ast::BinaryOperator::Add: return Constant{ValueType::Double, *a + *b};
                case ast::BinaryOperator::Subtract: return Constant{ValueType::Double, *a - *b};
                case ast::BinaryOperator::Multiply: return Constant{ValueType::Double, *a * *b};
                case ast::BinaryOperator::Divide: if (*b != 0.0) return Constant{ValueType::Double, *a / *b}; return std::nullopt;
                case ast::BinaryOperator::Equal: return Constant{ValueType::Bool, *a == *b};
                case ast::BinaryOperator::NotEqual: return Constant{ValueType::Bool, *a != *b};
                case ast::BinaryOperator::Less: return Constant{ValueType::Bool, *a < *b};
                case ast::BinaryOperator::LessEqual: return Constant{ValueType::Bool, *a <= *b};
                case ast::BinaryOperator::Greater: return Constant{ValueType::Bool, *a > *b};
                case ast::BinaryOperator::GreaterEqual: return Constant{ValueType::Bool, *a >= *b};
                default: return std::nullopt;
                }
            }
        }
        if (const auto* a = std::get_if<bool>(&left.value)) {
            if (const auto* b = std::get_if<bool>(&right.value)) {
                if (expression.op() == ast::BinaryOperator::LogicalAnd) return Constant{ValueType::Bool, *a && *b};
                if (expression.op() == ast::BinaryOperator::LogicalOr) return Constant{ValueType::Bool, *a || *b};
                if (expression.op() == ast::BinaryOperator::Equal) return Constant{ValueType::Bool, *a == *b};
                if (expression.op() == ast::BinaryOperator::NotEqual) return Constant{ValueType::Bool, *a != *b};
            }
        }
        if (std::holds_alternative<std::monostate>(left.value) &&
            std::holds_alternative<std::monostate>(right.value)) {
            if (expression.op() == ast::BinaryOperator::Equal) return Constant{ValueType::Bool, true};
            if (expression.op() == ast::BinaryOperator::NotEqual) return Constant{ValueType::Bool, false};
        }
        return std::nullopt;
    }

    void registerTypes() {
        // Pass one reserves every nominal type name so fields may refer to
        // structs/enums declared later in the file.
        for (const ast::DeclPtr& declaration : program_.declarations()) {
            if (const auto* structure = dynamic_cast<const ast::StructDecl*>(declaration.get())) {
                if (!fitsInstructionOperand(module_.structs.size())) {
                    report("too many struct types for bytecode format", structure->span());
                    continue;
                }
                const auto index = static_cast<std::uint32_t>(module_.structs.size());
                structIndices_.emplace(structure->name(), index);
                module_.structs.push_back(StructDescriptor{structure->name(), {}});
            } else if (const auto* enumeration = dynamic_cast<const ast::EnumDecl*>(declaration.get())) {
                if (!fitsInstructionOperand(module_.enums.size())) {
                    report("too many enum types for bytecode format", enumeration->span());
                    continue;
                }
                const auto index = static_cast<std::uint32_t>(module_.enums.size());
                enumIndices_.emplace(enumeration->name(), index);
                module_.enums.push_back(EnumDescriptor{enumeration->name(), {}});
            }
        }

        // Pass two fills members after all nominal type names are known.
        for (const ast::DeclPtr& declaration : program_.declarations()) {
            if (const auto* structure = dynamic_cast<const ast::StructDecl*>(declaration.get())) {
                const auto found = structIndices_.find(structure->name());
                if (found == structIndices_.end()) continue;
                auto& descriptor = module_.structs[found->second];
                auto& members = fieldIndices_[structure->name()];
                for (std::size_t fieldIndex = 0; fieldIndex < structure->fields().size(); ++fieldIndex) {
                    const ast::FieldDecl& field = structure->fields()[fieldIndex];
                    if (!fitsInstructionOperand(fieldIndex)) {
                        report("too many fields in struct '" + structure->name() + "'", field.span);
                        continue;
                    }
                    members.emplace(field.name, static_cast<std::uint32_t>(fieldIndex));
                    TypeInfo fieldType = typeInfoForName(field.type.name);
                    std::optional<std::uint32_t> defaultConstant;
                    if (field.defaultValue != nullptr) {
                        const auto constant = constantExpression(*field.defaultValue);
                        if (!constant) {
                            report("struct field default must be a compile-time constant in bytecode v0.7",
                                   field.span);
                        } else {
                            defaultConstant = addConstant(*constant, field.span);
                        }
                    }
                    descriptor.fields.push_back(FieldDescriptor{field.name, std::move(fieldType), defaultConstant});
                }
            } else if (const auto* enumeration = dynamic_cast<const ast::EnumDecl*>(declaration.get())) {
                const auto found = enumIndices_.find(enumeration->name());
                if (found == enumIndices_.end()) continue;
                auto& descriptor = module_.enums[found->second];
                for (const ast::EnumMember& member : enumeration->members()) {
                    descriptor.members.push_back(member.name);
                }
            }
        }
    }

    [[nodiscard]] TypeInfo typeInfoForName(const std::string& name) const {
        if (name == "int") return {ValueType::Int, {}};
        if (name == "float") return {ValueType::Float, {}};
        if (name == "double") return {ValueType::Double, {}};
        if (name == "bool") return {ValueType::Bool, {}};
        if (name == "string") return {ValueType::String, {}};
        if (name == "any") return {ValueType::Any, {}};
        if (name == "stream") return {ValueType::Stream, {}};
        if (name == "void") return {ValueType::Void, {}};
        if (structIndices_.find(name) != structIndices_.end()) return {ValueType::Struct, name};
        if (enumIndices_.find(name) != enumIndices_.end()) return {ValueType::Enum, name};
        return {ValueType::Invalid, name};
    }

    void registerGlobals() {
        for (const ast::DeclPtr& declaration : program_.declarations()) {
            const auto* global = dynamic_cast<const ast::GlobalVariableDecl*>(declaration.get());
            if (global == nullptr) continue;
            if (!fitsInstructionOperand(module_.globals.size())) {
                report("too many global variables for bytecode format", global->span());
                continue;
            }
            const ast::VariableSpec& variable = global->variable();
            const auto index = static_cast<std::uint32_t>(module_.globals.size());
            if (!globalIndices_.emplace(variable.name, index).second) {
                report("duplicate global variable '" + variable.name + "'", variable.span);
                continue;
            }
            module_.globals.push_back(GlobalSlot{
                variable.name, typeInfoForName(variable.type.name), variable.isMutable});
            globalDeclarations_.push_back(global);
        }
    }

    void registerFunctions() {
        for (const ast::DeclPtr& declaration : program_.declarations()) {
            const auto* function = dynamic_cast<const ast::FunctionDecl*>(declaration.get());
            if (function != nullptr && function->isPrototype()) {
                std::vector<ast::TypeRef> types;
                for (const ast::Parameter& parameter : function->parameters()) {
                    if (parameter.type.has_value()) types.push_back(*parameter.type);
                    else report("function prototype parameter is missing a type", parameter.span);
                }
                prototypeParameterTypes_.insert_or_assign(function->name(), std::move(types));
            }
        }

        for (const ast::DeclPtr& declaration : program_.declarations()) {
            const auto* function = dynamic_cast<const ast::FunctionDecl*>(declaration.get());
            if (function == nullptr || function->body() == nullptr) continue;

            std::vector<TypeInfo> parameterTypes;
            if (function->form() == ast::FunctionForm::TypedDefinition) {
                for (const ast::Parameter& parameter : function->parameters()) {
                    if (!parameter.type.has_value()) {
                        report("typed function definition is missing a parameter type", parameter.span);
                        parameterTypes.push_back({ValueType::Invalid, {}});
                    } else {
                        parameterTypes.push_back(typeInfoForName(parameter.type->name));
                    }
                }
            } else {
                const auto prototype = prototypeParameterTypes_.find(function->name());
                if (prototype == prototypeParameterTypes_.end()) {
                    report("signature implementation of '" + function->name() +
                               "' has no preceding typed prototype", function->span());
                } else {
                    for (const ast::TypeRef& parameterType : prototype->second) {
                        parameterTypes.push_back(typeInfoForName(parameterType.name));
                    }
                }
            }

            if (parameterTypes.size() != function->parameters().size()) {
                report("parameter count for function '" + function->name() + "' does not match its signature",
                       function->span());
            }
            if (!fitsInstructionOperand(module_.functions.size())) {
                report("too many functions for bytecode format", function->span());
                continue;
            }
            if (functionIndices_.find(function->name()) != functionIndices_.end()) {
                report("function '" + function->name() + "' has multiple definitions", function->span());
                continue;
            }

            const auto index = static_cast<std::uint32_t>(module_.functions.size());
            functionIndices_.emplace(function->name(), index);
            module_.functions.push_back(Function{
                .name = function->name(),
                .parameterTypes = std::move(parameterTypes),
                .returnType = typeInfoForName(function->returnType().name),
                .locals = {},
                .code = {},
                .debugLocations = {},
                .synthetic = false,
            });
            functionDefinitions_.push_back(function);
        }

        for (const auto& [name, parameterTypes] : prototypeParameterTypes_) {
            (void)parameterTypes;
            if (functionIndices_.find(name) == functionIndices_.end()) {
                report("function prototype '" + name + "' has no implementation in bytecode v0.7", {});
            }
        }

        const auto main = functionIndices_.find("main");
        if (main != functionIndices_.end()) module_.entryFunction = main->second;
    }

    void compileFunctions() {
        for (std::size_t index = 0; index < functionDefinitions_.size(); ++index) {
            const std::uint32_t functionIndex = static_cast<std::uint32_t>(index);
            FunctionCompiler compiler(
                *this, module_.functions[functionIndex], functionIndices_, globalIndices_,
                structIndices_, enumIndices_, fieldIndices_);
            compiler.compileFunction(*functionDefinitions_[index]);
        }
    }

    void compileGlobalInitializer() {
        if (globalDeclarations_.empty()) return;
        if (!fitsInstructionOperand(module_.functions.size())) {
            report("too many functions for global initializer", {});
            return;
        }
        const auto index = static_cast<std::uint32_t>(module_.functions.size());
        module_.functions.push_back(Function{
            .name = "__serenity_init_globals",
            .parameterTypes = {},
            .returnType = {ValueType::Void, {}},
            .locals = {},
            .code = {},
            .debugLocations = {},
            .synthetic = true,
        });
        module_.globalInitializerFunction = index;
        FunctionCompiler compiler(
            *this, module_.functions.back(), functionIndices_, globalIndices_,
            structIndices_, enumIndices_, fieldIndices_, true);
        compiler.compileGlobalInitializers(globalDeclarations_);
    }
};

FunctionCompiler::FunctionCompiler(
    ModuleBuilder& owner,
    Function& function,
    std::unordered_map<std::string, std::uint32_t>& functionIndices,
    const std::unordered_map<std::string, std::uint32_t>& globalIndices,
    const std::unordered_map<std::string, std::uint32_t>& structIndices,
    const std::unordered_map<std::string, std::uint32_t>& enumIndices,
    const std::unordered_map<std::string, std::unordered_map<std::string, std::uint32_t>>& fieldIndices,
    bool synthetic)
    : owner_(owner),
      function_(function),
      functionIndices_(functionIndices),
      globalIndices_(globalIndices),
      structIndices_(structIndices),
      enumIndices_(enumIndices),
      fieldIndices_(fieldIndices) {
    (void)synthetic;
}

void FunctionCompiler::report(std::string message, ast::SourceSpan span) {
    owner_.report(std::move(message), span);
}

std::size_t FunctionCompiler::emit(
    Opcode opcode, ValueType type, std::int32_t operand, std::int32_t extra, ast::SourceSpan span) {
    if (function_.code.size() >= static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        report("function exceeds the bytecode instruction index range", span);
        return function_.code.size();
    }
    function_.code.push_back(Instruction{opcode, type, 0, operand, extra});
    function_.debugLocations.push_back(span);
    return function_.code.size() - 1;
}

std::size_t FunctionCompiler::emit(Opcode opcode, ast::SourceSpan span) {
    return emit(opcode, ValueType::Invalid, 0, 0, span);
}

void FunctionCompiler::patchJump(std::size_t instruction, std::size_t target, ast::SourceSpan span) {
    if (instruction >= function_.code.size() || !fitsInstructionOperand(target)) {
        report("bytecode jump target exceeds the supported index range", span);
        return;
    }
    function_.code[instruction].operand = static_cast<std::int32_t>(target);
}

TypeInfo FunctionCompiler::typeInfo(const ast::TypeRef& type, ast::SourceSpan span) {
    if (type.name == "int") return {ValueType::Int, {}};
    if (type.name == "float") return {ValueType::Float, {}};
    if (type.name == "double") return {ValueType::Double, {}};
    if (type.name == "bool") return {ValueType::Bool, {}};
    if (type.name == "string") return {ValueType::String, {}};
    if (type.name == "any") return {ValueType::Any, {}};
    if (type.name == "stream") return {ValueType::Stream, {}};
    if (type.name == "void") return {ValueType::Void, {}};
    if (structIndices_.find(type.name) != structIndices_.end()) return {ValueType::Struct, type.name};
    if (enumIndices_.find(type.name) != enumIndices_.end()) return {ValueType::Enum, type.name};
    report("cannot generate bytecode for unknown type '" + type.name + "'", span);
    return {ValueType::Invalid, type.name};
}

semantic::Type FunctionCompiler::semanticType(const ast::Expression& expression) const {
    const auto found = owner_.semanticModel().expressionTypes.find(&expression);
    return found == owner_.semanticModel().expressionTypes.end()
        ? semantic::Type{TypeKind::Error, {}}
        : found->second;
}

TypeInfo FunctionCompiler::expressionType(const ast::Expression& expression) const {
    return toTypeInfo(semanticType(expression));
}

FunctionCompiler::Binding* FunctionCompiler::lookup(const std::string& name) {
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
        const auto found = scope->find(name);
        if (found != scope->end()) return &found->second;
    }
    const auto global = globalIndices_.find(name);
    if (global != globalIndices_.end()) {
        const auto& descriptor = owner_.module().globals[global->second];
        globalBindingScratch_ = Binding{true, global->second, descriptor.type, descriptor.isMutable};
        return &globalBindingScratch_;
    }
    return nullptr;
}

std::optional<std::uint32_t> FunctionCompiler::allocateLocal(
    const std::string& name, TypeInfo type, bool isMutable, bool isParameter, ast::SourceSpan span) {
    if (scopes_.empty()) scopes_.emplace_back();
    if (scopes_.back().find(name) != scopes_.back().end()) {
        report("duplicate local variable '" + name + "' during bytecode generation", span);
        return std::nullopt;
    }
    if (!fitsInstructionOperand(function_.locals.size())) {
        report("function has too many local variables for bytecode format", span);
        return std::nullopt;
    }
    const auto slot = static_cast<std::uint32_t>(function_.locals.size());
    function_.locals.push_back(LocalSlot{name, type, isMutable, isParameter});
    scopes_.back().emplace(name, Binding{false, slot, std::move(type), isMutable});
    return slot;
}

void FunctionCompiler::emitConstant(Constant constant, ast::SourceSpan span) {
    const std::uint32_t index = owner_.addConstant(std::move(constant), span);
    (void)emit(Opcode::PushConstant, ValueType::Invalid, static_cast<std::int32_t>(index), 0, span);
}

void FunctionCompiler::emitDefault(TypeInfo type, ast::SourceSpan span) {
    switch (type.kind) {
    case ValueType::Int: emitConstant(Constant{ValueType::Int, std::int64_t{0}}, span); break;
    case ValueType::Float: emitConstant(Constant{ValueType::Float, 0.0F}, span); break;
    case ValueType::Double: emitConstant(Constant{ValueType::Double, 0.0}, span); break;
    case ValueType::Bool: emitConstant(Constant{ValueType::Bool, false}, span); break;
    case ValueType::String:
    case ValueType::Struct:
    case ValueType::Enum:
    case ValueType::Null:
    case ValueType::Any:
    case ValueType::Stream:
        emitConstant(Constant{ValueType::Null, std::monostate{}}, span);
        break;
    case ValueType::Void:
        break;
    case ValueType::Invalid:
        report("cannot materialize a default value for this type", span);
        emitConstant(Constant{ValueType::Null, std::monostate{}}, span);
        break;
    }
}

void FunctionCompiler::load(const Binding& binding, ast::SourceSpan span) {
    if (binding.global) {
        (void)emit(Opcode::LoadGlobal, binding.type.kind, static_cast<std::int32_t>(binding.index), 0, span);
    } else {
        (void)emit(Opcode::LoadLocal, binding.type.kind, static_cast<std::int32_t>(binding.index), 0, span);
    }
}

void FunctionCompiler::store(const Binding& binding, ast::SourceSpan span) {
    if (binding.global) {
        (void)emit(Opcode::StoreGlobal, binding.type.kind, static_cast<std::int32_t>(binding.index), 0, span);
    } else {
        (void)emit(Opcode::StoreLocal, binding.type.kind, static_cast<std::int32_t>(binding.index), 0, span);
    }
}

void FunctionCompiler::compileCompoundOperator(
    ast::AssignmentOperator op, TypeInfo type, ast::SourceSpan span) {
    Opcode opcode = Opcode::Add;
    switch (op) {
    case ast::AssignmentOperator::Add: opcode = Opcode::Add; break;
    case ast::AssignmentOperator::Subtract: opcode = Opcode::Subtract; break;
    case ast::AssignmentOperator::Multiply: opcode = Opcode::Multiply; break;
    case ast::AssignmentOperator::Divide: opcode = Opcode::Divide; break;
    case ast::AssignmentOperator::Modulo: opcode = Opcode::Modulo; break;
    case ast::AssignmentOperator::Assign: return;
    }
    (void)emit(opcode, type.kind, 0, 0, span);
}

Opcode FunctionCompiler::binaryOpcode(ast::BinaryOperator op) const {
    switch (op) {
    case ast::BinaryOperator::Add: return Opcode::Add;
    case ast::BinaryOperator::Subtract: return Opcode::Subtract;
    case ast::BinaryOperator::Multiply: return Opcode::Multiply;
    case ast::BinaryOperator::Divide: return Opcode::Divide;
    case ast::BinaryOperator::Modulo: return Opcode::Modulo;
    case ast::BinaryOperator::Equal: return Opcode::CompareEqual;
    case ast::BinaryOperator::NotEqual: return Opcode::CompareNotEqual;
    case ast::BinaryOperator::Less: return Opcode::CompareLess;
    case ast::BinaryOperator::Greater: return Opcode::CompareGreater;
    case ast::BinaryOperator::LessEqual: return Opcode::CompareLessEqual;
    case ast::BinaryOperator::GreaterEqual: return Opcode::CompareGreaterEqual;
    case ast::BinaryOperator::BitwiseAnd: return Opcode::BitwiseAnd;
    case ast::BinaryOperator::BitwiseOr: return Opcode::BitwiseOr;
    case ast::BinaryOperator::BitwiseXor: return Opcode::BitwiseXor;
    case ast::BinaryOperator::ShiftLeft: return Opcode::ShiftLeft;
    case ast::BinaryOperator::ShiftRight: return Opcode::ShiftRight;
    case ast::BinaryOperator::LogicalAnd: return Opcode::Nop;
    case ast::BinaryOperator::LogicalOr: return Opcode::Nop;
    }
    return Opcode::Nop;
}

void FunctionCompiler::compileFunction(const ast::FunctionDecl& declaration) {
    scopes_.emplace_back();
    if (declaration.parameters().size() != function_.parameterTypes.size()) {
        report("function parameter metadata does not match its definition", declaration.span());
    }
    const std::size_t count = std::min(declaration.parameters().size(), function_.parameterTypes.size());
    for (std::size_t index = 0; index < count; ++index) {
        const ast::Parameter& parameter = declaration.parameters()[index];
        if (!parameter.name.has_value()) {
            report("function parameter has no name", parameter.span);
            continue;
        }
        (void)allocateLocal(*parameter.name, function_.parameterTypes[index], false, true, parameter.span);
    }

    if (declaration.body() != nullptr) compileBlock(*declaration.body(), false);
    // The semantic analyzer currently does not prove that every control-flow
    // path returns. Emit a type-correct fallback so malformed fallthrough never
    // leaves the interpreter's operand stack without a return value.
    emitDefault(function_.returnType, declaration.span());
    (void)emit(Opcode::Return, function_.returnType.kind, 0, 0, declaration.span());
}

void FunctionCompiler::compileGlobalInitializers(
    const std::vector<const ast::GlobalVariableDecl*>& globals) {
    for (const ast::GlobalVariableDecl* declaration : globals) {
        const auto found = globalIndices_.find(declaration->variable().name);
        if (found == globalIndices_.end()) continue;
        const GlobalSlot& global = owner_.module().globals[found->second];
        emitDefault(global.type, declaration->span());
        (void)emit(Opcode::StoreGlobal, global.type.kind,
                   static_cast<std::int32_t>(found->second), 0, declaration->span());
    }
    // Initialize all globals to their defaults before evaluating any explicit
    // initializer, so a later global referenced by an earlier initializer has
    // a defined default value.
    for (const ast::GlobalVariableDecl* declaration : globals) {
        const ast::VariableSpec& variable = declaration->variable();
        if (variable.initializer == nullptr) continue;
        const auto found = globalIndices_.find(variable.name);
        if (found == globalIndices_.end()) continue;
        compileExpression(*variable.initializer);
        const GlobalSlot& global = owner_.module().globals[found->second];
        (void)emit(Opcode::StoreGlobal, global.type.kind,
                   static_cast<std::int32_t>(found->second), 0, variable.span);
    }
    (void)emit(Opcode::Return, ValueType::Void, 0, 0, {});
}

void FunctionCompiler::compileBlock(const ast::BlockStmt& block, bool createScope) {
    if (createScope) scopes_.emplace_back();
    for (const ast::StmtPtr& statement : block.statements()) {
        if (statement != nullptr) compileStatement(*statement);
    }
    if (createScope) scopes_.pop_back();
}

void FunctionCompiler::compileStatement(const ast::Statement& statement) {
    if (const auto* block = dynamic_cast<const ast::BlockStmt*>(&statement)) {
        compileBlock(*block, true);
    } else if (const auto* variable = dynamic_cast<const ast::VariableDeclStmt*>(&statement)) {
        compileVariable(variable->variable(), variable->span());
    } else if (const auto* assignment = dynamic_cast<const ast::AssignmentStmt*>(&statement)) {
        compileAssignment(*assignment);
    } else if (const auto* expression = dynamic_cast<const ast::ExpressionStmt*>(&statement)) {
        compileExpression(expression->expression());
        if (expressionType(expression->expression()).kind != ValueType::Void) {
            (void)emit(Opcode::Pop, expression->span());
        }
    } else if (const auto* returned = dynamic_cast<const ast::ReturnStmt*>(&statement)) {
        compileReturn(*returned);
    } else if (const auto* conditional = dynamic_cast<const ast::IfStmt*>(&statement)) {
        compileIf(*conditional);
    } else if (const auto* loop = dynamic_cast<const ast::WhileStmt*>(&statement)) {
        compileWhile(*loop);
    } else if (dynamic_cast<const ast::BreakStmt*>(&statement) != nullptr) {
        if (loops_.empty()) {
            report("'break' has no enclosing loop during bytecode generation", statement.span());
        } else {
            loops_.back().breakJumps.push_back(emit(Opcode::Jump, statement.span()));
        }
    } else if (dynamic_cast<const ast::ContinueStmt*>(&statement) != nullptr) {
        if (loops_.empty()) {
            report("'continue' has no enclosing loop during bytecode generation", statement.span());
        } else {
            const std::size_t jump = emit(Opcode::Jump, statement.span());
            patchJump(jump, loops_.back().continueTarget, statement.span());
        }
    } else {
        report("unsupported statement in bytecode generator", statement.span());
    }
}

void FunctionCompiler::compileVariable(const ast::VariableSpec& variable, ast::SourceSpan span) {
    const TypeInfo type = typeInfo(variable.type, variable.span);
    const auto slot = allocateLocal(variable.name, type, variable.isMutable, false, span);
    if (!slot.has_value()) return;

    const Binding binding{false, *slot, type, variable.isMutable};
    // Serenity default-initializes a local before evaluating its initializer.
    emitDefault(type, span);
    store(binding, span);
    if (variable.initializer != nullptr) {
        compileExpression(*variable.initializer);
        store(binding, span);
    }
}

void FunctionCompiler::compileAssignment(const ast::AssignmentStmt& assignment) {
    const ast::Expression& target = assignment.target();
    if (const auto* identifier = dynamic_cast<const ast::IdentifierExpr*>(&target)) {
        const Binding* foundBinding = lookup(identifier->name());
        if (foundBinding == nullptr) {
            report("unknown assignment target '" + identifier->name() + "'", target.span());
            return;
        }
        const Binding binding = *foundBinding;
        if (assignment.op() != ast::AssignmentOperator::Assign) {
            load(binding, target.span());
            compileExpression(assignment.value());
            compileCompoundOperator(assignment.op(), binding.type, assignment.span());
        } else {
            compileExpression(assignment.value());
        }
        store(binding, assignment.span());
        return;
    }

    if (const auto* member = dynamic_cast<const ast::MemberAccessExpr*>(&target)) {
        const semantic::Type objectType = semanticType(member->object());
        if (objectType.kind != TypeKind::Struct) {
            report("bytecode field assignment requires a struct value", target.span());
            return;
        }
        const auto structure = fieldIndices_.find(objectType.name);
        if (structure == fieldIndices_.end()) {
            report("unknown struct type '" + objectType.name + "' during field assignment", target.span());
            return;
        }
        const auto field = structure->second.find(member->member());
        if (field == structure->second.end()) {
            report("unknown field '" + member->member() + "' during bytecode generation", target.span());
            return;
        }

        compileExpression(member->object());
        if (assignment.op() != ast::AssignmentOperator::Assign) {
            (void)emit(Opcode::Duplicate, member->object().span());
            (void)emit(Opcode::GetField, ValueType::Invalid,
                       static_cast<std::int32_t>(field->second), 0, target.span());
            compileExpression(assignment.value());
            const TypeInfo fieldType = owner_.module().structs[structIndices_.at(objectType.name)]
                .fields[field->second].type;
            compileCompoundOperator(assignment.op(), fieldType, assignment.span());
        } else {
            compileExpression(assignment.value());
        }
        (void)emit(Opcode::SetField, ValueType::Invalid,
                   static_cast<std::int32_t>(field->second), 0, assignment.span());
        return;
    }

    report("unsupported assignment target in bytecode generator", target.span());
}

void FunctionCompiler::compileReturn(const ast::ReturnStmt& statement) {
    compileExpression(statement.value());
    (void)emit(Opcode::Return, function_.returnType.kind, 0, 0, statement.span());
}

void FunctionCompiler::compileIf(const ast::IfStmt& statement) {
    compileExpression(statement.condition());
    const std::size_t falseJump = emit(Opcode::JumpIfFalse, statement.condition().span());
    compileBlock(statement.thenBranch(), true);

    if (statement.elseBranch() != nullptr) {
        const std::size_t endJump = emit(Opcode::Jump, statement.span());
        patchJump(falseJump, function_.code.size(), statement.span());
        compileStatement(*statement.elseBranch());
        patchJump(endJump, function_.code.size(), statement.span());
    } else {
        patchJump(falseJump, function_.code.size(), statement.span());
    }
}

void FunctionCompiler::compileWhile(const ast::WhileStmt& statement) {
    const std::size_t loopStart = function_.code.size();
    compileExpression(statement.condition());
    const std::size_t exitJump = emit(Opcode::JumpIfFalse, statement.condition().span());

    loops_.push_back(LoopContext{loopStart, {}});
    compileBlock(statement.body(), true);
    const std::size_t backJump = emit(Opcode::Jump, statement.span());
    patchJump(backJump, loopStart, statement.span());

    const std::size_t loopEnd = function_.code.size();
    patchJump(exitJump, loopEnd, statement.span());
    for (const std::size_t breakJump : loops_.back().breakJumps) {
        patchJump(breakJump, loopEnd, statement.span());
    }
    loops_.pop_back();
}

void FunctionCompiler::compileExpression(const ast::Expression& expression) {
    if (const auto* integer = dynamic_cast<const ast::IntegerLiteralExpr*>(&expression)) {
        const auto value = parseInteger(integer->spelling());
        if (!value) {
            report("integer literal is outside the bytecode integer range", expression.span());
            emitConstant(Constant{ValueType::Int, std::int64_t{0}}, expression.span());
        } else {
            emitConstant(Constant{ValueType::Int, *value}, expression.span());
        }
    } else if (const auto* floating = dynamic_cast<const ast::FloatingLiteralExpr*>(&expression)) {
        const ValueType type = expressionType(expression).kind;
        if (type == ValueType::Float) {
            const auto value = parseFloat(floating->spelling());
            if (!value) {
                report("float literal is outside the supported range", expression.span());
                emitConstant(Constant{ValueType::Float, 0.0F}, expression.span());
            } else {
                emitConstant(Constant{ValueType::Float, *value}, expression.span());
            }
        } else {
            const auto value = parseDouble(floating->spelling());
            if (!value) {
                report("double literal is outside the supported range", expression.span());
                emitConstant(Constant{ValueType::Double, 0.0}, expression.span());
            } else {
                emitConstant(Constant{ValueType::Double, *value}, expression.span());
            }
        }
    } else if (const auto* string = dynamic_cast<const ast::StringLiteralExpr*>(&expression)) {
        emitConstant(Constant{ValueType::String, string->value()}, expression.span());
    } else if (const auto* boolean = dynamic_cast<const ast::BooleanLiteralExpr*>(&expression)) {
        emitConstant(Constant{ValueType::Bool, boolean->value()}, expression.span());
    } else if (dynamic_cast<const ast::NullLiteralExpr*>(&expression) != nullptr) {
        emitConstant(Constant{ValueType::Null, std::monostate{}}, expression.span());
    } else if (const auto* identifier = dynamic_cast<const ast::IdentifierExpr*>(&expression)) {
        const Binding* binding = lookup(identifier->name());
        if (binding != nullptr) {
            load(*binding, expression.span());
        } else if (identifier->name() == "stdout") {
            (void)emit(Opcode::PushStream, ValueType::Stream, 0, 0, expression.span());
        } else if (identifier->name() == "stdin") {
            (void)emit(Opcode::PushStream, ValueType::Stream, 1, 0, expression.span());
        } else if (identifier->name() == "stderr") {
            (void)emit(Opcode::PushStream, ValueType::Stream, 2, 0, expression.span());
        } else {
            report("unresolved identifier '" + identifier->name() + "' during bytecode generation",
                   expression.span());
        }
    } else if (const auto* unary = dynamic_cast<const ast::UnaryExpr*>(&expression)) {
        compileExpression(unary->operand());
        switch (unary->op()) {
        case ast::UnaryOperator::Plus: break;
        case ast::UnaryOperator::Minus:
            (void)emit(Opcode::Negate, expressionType(unary->operand()).kind, 0, 0, expression.span());
            break;
        case ast::UnaryOperator::LogicalNot:
            (void)emit(Opcode::LogicalNot, ValueType::Bool, 0, 0, expression.span());
            break;
        case ast::UnaryOperator::BitwiseNot:
            (void)emit(Opcode::BitwiseNot, ValueType::Int, 0, 0, expression.span());
            break;
        }
    } else if (const auto* binary = dynamic_cast<const ast::BinaryExpr*>(&expression)) {
        if (binary->op() == ast::BinaryOperator::LogicalAnd) {
            compileExpression(binary->left());
            const std::size_t falseJump = emit(Opcode::JumpIfFalse, binary->left().span());
            compileExpression(binary->right());
            const std::size_t endJump = emit(Opcode::Jump, expression.span());
            patchJump(falseJump, function_.code.size(), expression.span());
            emitConstant(Constant{ValueType::Bool, false}, expression.span());
            patchJump(endJump, function_.code.size(), expression.span());
        } else if (binary->op() == ast::BinaryOperator::LogicalOr) {
            compileExpression(binary->left());
            const std::size_t evaluateRight = emit(Opcode::JumpIfFalse, binary->left().span());
            emitConstant(Constant{ValueType::Bool, true}, expression.span());
            const std::size_t endJump = emit(Opcode::Jump, expression.span());
            patchJump(evaluateRight, function_.code.size(), expression.span());
            compileExpression(binary->right());
            patchJump(endJump, function_.code.size(), expression.span());
        } else {
            compileExpression(binary->left());
            compileExpression(binary->right());
            ValueType operandType = expressionType(binary->left()).kind;
            if (operandType == ValueType::Null) operandType = expressionType(binary->right()).kind;
            (void)emit(binaryOpcode(binary->op()), operandType, 0, 0, expression.span());
        }
    } else if (const auto* call = dynamic_cast<const ast::CallExpr*>(&expression)) {
        compileCall(*call);
    } else if (const auto* member = dynamic_cast<const ast::MemberAccessExpr*>(&expression)) {
        compileMemberAccess(*member);
    } else if (const auto* initializer = dynamic_cast<const ast::StructInitializerExpr*>(&expression)) {
        compileStructInitializer(*initializer);
    } else {
        report("unsupported expression in bytecode generator", expression.span());
    }
}

void FunctionCompiler::compileCall(const ast::CallExpr& expression) {
    const std::string& callee = expression.callee();
    if (callee == "write") {
        if (expression.arguments().size() != 2 ||
            expression.arguments()[0] == nullptr || expression.arguments()[1] == nullptr) {
            report("built-in write requires exactly two arguments", expression.span());
            return;
        }
        compileExpression(*expression.arguments()[0]);
        compileExpression(*expression.arguments()[1]);
        (void)emit(Opcode::StreamWrite, expression.span());
        return;
    }
    if (callee == "read") {
        if (expression.arguments().size() != 1 || expression.arguments()[0] == nullptr) {
            report("built-in read requires exactly one argument", expression.span());
            return;
        }
        compileExpression(*expression.arguments()[0]);
        (void)emit(Opcode::StreamRead, expression.span());
        return;
    }

    const auto function = functionIndices_.find(callee);
    if (function != functionIndices_.end()) {
        for (const ast::ExprPtr& argument : expression.arguments()) {
            if (argument != nullptr) compileExpression(*argument);
        }
        (void)emit(Opcode::Call, ValueType::Invalid,
                   static_cast<std::int32_t>(function->second),
                   static_cast<std::int32_t>(expression.arguments().size()), expression.span());
        return;
    }

    if (callee == "print" || callee == "println") {
        if (expression.arguments().size() != 1 || expression.arguments().front() == nullptr) {
            report("built-in output call has an invalid argument list", expression.span());
            return;
        }
        const ast::Expression& argument = *expression.arguments().front();
        compileExpression(argument);
        const ValueType type = expressionType(argument).kind;
        (void)emit(callee == "print" ? Opcode::Print : Opcode::Println,
                   type, 0, 0, expression.span());
        return;
    }

    report("call to function '" + callee + "' has no bytecode implementation", expression.span());
}

void FunctionCompiler::compileMemberAccess(const ast::MemberAccessExpr& expression) {
    if (const auto* qualifier = dynamic_cast<const ast::IdentifierExpr*>(&expression.object())) {
        const auto enumeration = enumIndices_.find(qualifier->name());
        if (enumeration != enumIndices_.end()) {
            const EnumDescriptor& descriptor = owner_.module().enums[enumeration->second];
            for (std::size_t index = 0; index < descriptor.members.size(); ++index) {
                if (descriptor.members[index] == expression.member()) {
                    (void)emit(Opcode::PushEnum, ValueType::Enum,
                               static_cast<std::int32_t>(enumeration->second),
                               static_cast<std::int32_t>(index), expression.span());
                    return;
                }
            }
            report("unknown enum member '" + expression.member() + "' during bytecode generation",
                   expression.span());
            return;
        }
    }

    const semantic::Type objectType = semanticType(expression.object());
    if (objectType.kind != TypeKind::Struct) {
        report("member access in bytecode requires a struct instance", expression.span());
        return;
    }
    const auto structure = fieldIndices_.find(objectType.name);
    if (structure == fieldIndices_.end()) {
        report("unknown struct type '" + objectType.name + "' during bytecode generation", expression.span());
        return;
    }
    const auto field = structure->second.find(expression.member());
    if (field == structure->second.end()) {
        report("unknown field '" + expression.member() + "' during bytecode generation", expression.span());
        return;
    }
    compileExpression(expression.object());
    (void)emit(Opcode::GetField, ValueType::Invalid,
               static_cast<std::int32_t>(field->second), 0, expression.span());
}

void FunctionCompiler::compileStructInitializer(const ast::StructInitializerExpr& expression) {
    const semantic::Type type = semanticType(expression);
    if (type.kind != TypeKind::Struct) {
        report("struct initializer has no resolved struct type", expression.span());
        return;
    }
    const auto structure = structIndices_.find(type.name);
    if (structure == structIndices_.end()) {
        report("unknown struct type '" + type.name + "' during bytecode generation", expression.span());
        return;
    }
    const auto fields = fieldIndices_.find(type.name);
    if (fields == fieldIndices_.end()) {
        report("struct field metadata is missing for '" + type.name + "'", expression.span());
        return;
    }

    (void)emit(Opcode::NewStruct, ValueType::Struct,
               static_cast<std::int32_t>(structure->second), 0, expression.span());
    for (const ast::DesignatedInitializer& initializer : expression.fields()) {
        if (initializer.value == nullptr) continue;
        const auto field = fields->second.find(initializer.fieldName);
        if (field == fields->second.end()) continue;
        (void)emit(Opcode::Duplicate, initializer.span);
        compileExpression(*initializer.value);
        (void)emit(Opcode::SetField, ValueType::Invalid,
                   static_cast<std::int32_t>(field->second), 0, initializer.span);
    }
}

} // namespace

BytecodeGenerator::BytecodeGenerator(
    const ast::Program& program, const semantic::SemanticModel& semanticModel)
    : program_(program), semanticModel_(semanticModel) {}

BytecodeResult BytecodeGenerator::generate() {
    ModuleBuilder builder(program_, semanticModel_);
    return builder.build();
}

} // namespace serenity::bytecode
