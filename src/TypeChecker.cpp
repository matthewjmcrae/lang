#include "noria/TypeChecker.hpp"

#include "noria/Builtins.hpp"
#include "noria/Constraints.hpp"
#include "noria/Diagnostic.hpp"
#include "noria/Monomorphize.hpp"
#include "noria/SemanticTables.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace noria {

  namespace {

    [[noreturn]] void unsupportedExpressionInStatementVisitor() {
      throw CompileError("typecheck: internal error: expression visited by statement visitor");
    }

    [[noreturn]] void unsupportedStatementInExpressionVisitor() {
      throw CompileError("typecheck: internal error: statement visited by expression visitor");
    }

    bool isScalarWitnessType(const Type& type) {
      return type == Type::i32() || type == Type::f64() || type == Type::boolean() ||
             type == Type::str();
    }

    std::optional<Type> firstNonImplTagTypeArg(const std::vector<Type>& typeArgs) {
      for (const Type& typeArg : typeArgs) {
        if (typeArg.kind != TypeKind::ImplTag) {
          return typeArg;
        }
      }
      return std::nullopt;
    }

    bool structSpecializationsMatch(const Type& left, const Type& right) {
      if (left.kind != TypeKind::Struct || right.kind != TypeKind::Struct) {
        return false;
      }

      if (left.structName == right.structName && left.typeArgs == right.typeArgs) {
        return true;
      }

      if (left.typeArgs.empty() && !right.typeArgs.empty()) {
        return left.structName == mangleSpecialization(right.structName, right.typeArgs);
      }

      if (right.typeArgs.empty() && !left.typeArgs.empty()) {
        return right.structName == mangleSpecialization(left.structName, left.typeArgs);
      }

      return false;
    }

    bool allTypeParamsSubstituted(const Type& type, const Substitution& substitution) {
      if (type.kind == TypeKind::TypeParam) {
        return substitution.contains(type.typeParamName);
      }

      if (type.kind == TypeKind::Array && type.element) {
        return allTypeParamsSubstituted(*type.element, substitution);
      }

      if (type.kind == TypeKind::Struct) {
        for (const Type& typeArg : type.typeArgs) {
          if (!allTypeParamsSubstituted(typeArg, substitution)) {
            return false;
          }
        }
      }

      return true;
    }

    bool sameGenericPublicApi(const ast::Function& left, const ast::Function& right) {
      if (left.typeParams.size() != right.typeParams.size()) {
        return false;
      }
      for (std::size_t index{}; index < left.typeParams.size(); ++index) {
        if (left.typeParams[index].name != right.typeParams[index].name) {
          return false;
        }
      }
      if (left.parameters.size() != right.parameters.size()) {
        return false;
      }
      for (std::size_t index{}; index < left.parameters.size(); ++index) {
        if (left.parameters[index].type != right.parameters[index].type) {
          return false;
        }
      }
      return left.returnType == right.returnType;
    }

    std::optional<ImplementationTag> findImplTag(const std::vector<Type>& typeArgs) {
      for (const Type& typeArg : typeArgs) {
        if (typeArg.kind == TypeKind::ImplTag) {
          return typeArg.implTag;
        }
      }
      return std::nullopt;
    }

    void rejectStructArrayElement(const Type& elementType, SourceLocation location) {
      if (elementType.kind == TypeKind::Struct) {
        throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                            "array element type cannot be a struct"));
      }
    }

    const ast::Function*
    selectGenericImplementation(const ast::Module& module, const std::vector<std::size_t>& family,
                                std::optional<ImplementationTag> callTag,
                                std::string_view functionName, SourceLocation location) {
      if (family.empty()) {
        return nullptr;
      }

      if (family.size() == 1 && !module.functions.at(family.front()).implTag) {
        return &module.functions.at(family.front());
      }

      if (!callTag) {
        throw CompileError(
            formatDiagnostic(location, DiagnosticStage::TypeCheck,
                             "cannot select implementation of '" + std::string(functionName) +
                                 "' without an implementation tag in inferred type arguments"));
      }

      for (std::size_t candidateIndex : family) {
        const ast::Function& candidate = module.functions.at(candidateIndex);
        if (candidate.implTag && *candidate.implTag == *callTag) {
          return &candidate;
        }
      }

      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                          "no implementation of '" + std::string(functionName) +
                                              "' for tag '" +
                                              std::string(implementationTagName(*callTag)) + "'"));
    }

    bool containsUnboundTypeParam(const Type& type) {
      if (type.kind == TypeKind::TypeParam) {
        return true;
      }

      if (type.kind == TypeKind::Array) {
        return type.element && containsUnboundTypeParam(*type.element);
      }

      if (type.kind == TypeKind::Struct) {
        for (const Type& typeArg : type.typeArgs) {
          if (containsUnboundTypeParam(typeArg)) {
            return true;
          }
        }
      }

      return false;
    }

  } // namespace

  TypeChecker::StatementVisitor::StatementVisitor(TypeChecker& checker, Type expectedReturnType)
      : checker_(checker), expectedReturnType_(expectedReturnType) {}

  void TypeChecker::StatementVisitor::visit(const ast::LetStatement& letStatement) {
    const bool allowInternal = checker_.isStdlibContext();
    std::optional<Type> declaredType = letStatement.declaredType;
    if (declaredType) {
      checker_.requireKnownType(*declaredType, letStatement.location, nullptr, false,
                                allowInternal);
    }

    Type localType;
    if (letStatement.initializer) {
      const Type initializerType = checker_.checkRvalue(*letStatement.initializer, declaredType);
      if (declaredType) {
        localType = *declaredType;
        if (!checker_.isAssignable(localType, initializerType)) {
          throw CompileError(
              formatDiagnostic(letStatement.initializer->location, DiagnosticStage::TypeCheck,
                               "cannot initialize '" + letStatement.name + "' of type " +
                                   localType.name() + " with " + initializerType.name()));
        }
      } else {
        localType = initializerType;
        if (localType == Type::voidType()) {
          throw CompileError(formatDiagnostic(
              letStatement.initializer->location, DiagnosticStage::TypeCheck,
              "cannot infer local variable '" + letStatement.name + "' from void initializer"));
        }
        checker_.requireKnownType(localType, letStatement.location, nullptr, false, allowInternal);
      }
    } else {
      if (!declaredType) {
        throw CompileError(formatDiagnostic(
            letStatement.location, DiagnosticStage::TypeCheck,
            "local declaration '" + letStatement.name + "' requires a type or initializer"));
      }
      localType = *declaredType;
    }

    if (localType == Type::voidType()) {
      throw CompileError(formatDiagnostic(letStatement.location, DiagnosticStage::TypeCheck,
                                          "local variable '" + letStatement.name +
                                              "' cannot have type void"));
    }

    if (!checker_.declareLocal(letStatement.name, localType)) {
      throw CompileError(formatDiagnostic(letStatement.location, DiagnosticStage::TypeCheck,
                                          "duplicate local variable '" + letStatement.name + "'"));
    }

    returned_ = false;
  }

  void TypeChecker::StatementVisitor::visit(const ast::AssignmentStatement& assignmentStatement) {
    const auto place = checker_.checkPlace(*assignmentStatement.lhs);
    const Type valueType = checker_.checkRvalue(*assignmentStatement.rhs);

    if (!checker_.isAssignable(place.type, valueType)) {
      throw CompileError(formatDiagnostic(assignmentStatement.rhs->location,
                                          DiagnosticStage::TypeCheck,
                                          "cannot assign " + valueType.name() + " to variable '" +
                                              place.name + "' of type " + place.type.name()));
    }

    returned_ = false;
  }

  void TypeChecker::StatementVisitor::visit(const ast::ReturnStatement& returnStatement) {
    const Type returnType = checker_.checkRvalue(*returnStatement.expression, expectedReturnType_);

    if (!checker_.isAssignable(expectedReturnType_, returnType)) {
      throw CompileError(
          formatDiagnostic(returnStatement.expression->location, DiagnosticStage::TypeCheck,
                           "return type " + returnType.name() + " does not match expected " +
                               expectedReturnType_.name()));
    }

    returned_ = true;
  }

  void TypeChecker::StatementVisitor::visit(const ast::IfStatement& ifStatement) {
    const Type conditionType = checker_.checkRvalue(*ifStatement.condition);
    if (conditionType != Type::boolean()) {
      throw CompileError(
          formatDiagnostic(ifStatement.condition->location, DiagnosticStage::TypeCheck,
                           "if condition must be bool, got " + conditionType.name()));
    }

    checker_.pushScope();
    const bool thenReturns = checker_.checkStatements(ifStatement.thenBranch, expectedReturnType_);
    checker_.popScope();

    checker_.pushScope();
    const bool elseReturns = checker_.checkStatements(ifStatement.elseBranch, expectedReturnType_);
    checker_.popScope();

    returned_ = thenReturns && elseReturns;
  }

  void TypeChecker::StatementVisitor::visit(const ast::WhileStatement& whileStatement) {
    const Type conditionType = checker_.checkRvalue(*whileStatement.condition);
    if (conditionType != Type::boolean()) {
      throw CompileError(
          formatDiagnostic(whileStatement.condition->location, DiagnosticStage::TypeCheck,
                           "while condition must be bool, got " + conditionType.name()));
    }

    checker_.pushScope();
    checker_.checkStatements(whileStatement.body, expectedReturnType_);
    checker_.popScope();
    returned_ = false;
  }

  void TypeChecker::StatementVisitor::visit(const ast::ExpressionStatement& expressionStatement) {
    CallExpressionProbe probe;
    expressionStatement.expression->accept(probe);
    if (!probe.isCallExpression()) {
      throw CompileError(formatDiagnostic(expressionStatement.location, DiagnosticStage::TypeCheck,
                                          "expression statement must be a function call"));
    }

    const Type expressionType = checker_.checkRvalue(*expressionStatement.expression);
    if (expressionType != Type::voidType()) {
      throw CompileError(formatDiagnostic(expressionStatement.expression->location,
                                          DiagnosticStage::TypeCheck,
                                          "expression statement must call a void builtin"));
    }

    returned_ = false;
  }

  void TypeChecker::StatementVisitor::visit(const ast::IntegerLiteral&) {
    unsupportedExpressionInStatementVisitor();
  }
  void TypeChecker::StatementVisitor::visit(const ast::FloatLiteral&) {
    unsupportedExpressionInStatementVisitor();
  }
  void TypeChecker::StatementVisitor::visit(const ast::StringLiteral&) {
    unsupportedExpressionInStatementVisitor();
  }
  void TypeChecker::StatementVisitor::visit(const ast::BoolLiteral&) {
    unsupportedExpressionInStatementVisitor();
  }
  void TypeChecker::StatementVisitor::visit(const ast::UnaryExpression&) {
    unsupportedExpressionInStatementVisitor();
  }
  void TypeChecker::StatementVisitor::visit(const ast::CastExpression&) {
    unsupportedExpressionInStatementVisitor();
  }
  void TypeChecker::StatementVisitor::visit(const ast::BinaryExpression&) {
    unsupportedExpressionInStatementVisitor();
  }
  void TypeChecker::StatementVisitor::visit(const ast::IdentifierExpression&) {
    unsupportedExpressionInStatementVisitor();
  }
  void TypeChecker::StatementVisitor::visit(const ast::CallExpression&) {
    unsupportedExpressionInStatementVisitor();
  }
  void TypeChecker::StatementVisitor::visit(const ast::ArrayLiteral&) {
    unsupportedExpressionInStatementVisitor();
  }
  void TypeChecker::StatementVisitor::visit(const ast::IndexExpression&) {
    unsupportedExpressionInStatementVisitor();
  }
  void TypeChecker::StatementVisitor::visit(const ast::StructLiteral&) {
    unsupportedExpressionInStatementVisitor();
  }
  void TypeChecker::StatementVisitor::visit(const ast::FieldAccessExpression&) {
    unsupportedExpressionInStatementVisitor();
  }

  TypeChecker::ExpressionVisitor::ExpressionVisitor(TypeChecker& checker,
                                                    std::optional<Type> expectedType)
      : checker_(checker), expectedType_(std::move(expectedType)) {}

  void TypeChecker::ExpressionVisitor::visit(const ast::IntegerLiteral& integer) {
    if (integer.value < std::numeric_limits<std::int32_t>::min() ||
        integer.value > std::numeric_limits<std::int32_t>::max()) {
      throw CompileError(formatDiagnostic(integer.location, DiagnosticStage::TypeCheck,
                                          "integer literal out of i32 range"));
    }
    result_ = Type::i32();
  }

  void TypeChecker::ExpressionVisitor::visit(const ast::FloatLiteral&) {
    result_ = Type::f64();
  }

  void TypeChecker::ExpressionVisitor::visit(const ast::StringLiteral&) {
    result_ = Type::str();
  }

  void TypeChecker::ExpressionVisitor::visit(const ast::BoolLiteral&) {
    result_ = Type::boolean();
  }

  void TypeChecker::ExpressionVisitor::visit(const ast::IdentifierExpression& identifier) {
    result_ = checker_.lookupLocal(identifier.name, identifier.location);
  }

  void TypeChecker::ExpressionVisitor::visit(const ast::BinaryExpression& binary) {
    const Type left = checker_.checkRvalue(*binary.left);
    const Type right = checker_.checkRvalue(*binary.right);
    result_ = checker_.checkBinaryExpression(binary, left, right);
  }

  Type TypeChecker::checkBinaryExpression(const ast::BinaryExpression& binary, const Type& left,
                                          const Type& right) const {
    if (left.kind == TypeKind::RawPtr || right.kind == TypeKind::RawPtr) {
      throw CompileError(formatDiagnostic(binary.location, DiagnosticStage::TypeCheck,
                                          "invalid operation on __rt_ptr"));
    }

    using BinaryCheck = Type (TypeChecker::*)(const ast::BinaryExpression&, const Type&,
                                              const Type&) const;
    static const std::unordered_map<BinaryTypeCheckRule, BinaryCheck,
                                    EnumHash<BinaryTypeCheckRule>>
        checks = {
            {BinaryTypeCheckRule::Logical, &TypeChecker::checkLogicalBinaryExpression},
            {BinaryTypeCheckRule::Numeric, &TypeChecker::checkAdditiveBinaryExpression},
            {BinaryTypeCheckRule::Integer, &TypeChecker::checkIntegerBinaryExpression},
            {BinaryTypeCheckRule::OrderedComparison,
             &TypeChecker::checkOrderedComparisonExpression},
            {BinaryTypeCheckRule::Equality, &TypeChecker::checkEqualityExpression},
        };

    const BinaryOperatorInfo* info = binaryOperatorInfo(binary.op);
    if (info == nullptr) {
      throw CompileError("typecheck: internal error: unknown binary operator");
    }

    const auto check = checks.find(info->typeCheckRule);
    if (check == checks.end()) {
      throw CompileError("typecheck: internal error: unknown binary type-check rule");
    }

    return (this->*check->second)(binary, left, right);
  }

  Type TypeChecker::checkLogicalBinaryExpression(const ast::BinaryExpression& binary,
                                                 const Type& left, const Type& right) const {
    if (left != Type::boolean() || right != Type::boolean()) {
      throw CompileError(formatDiagnostic(binary.location, DiagnosticStage::TypeCheck,
                                          "logical operator requires bool operands, got " +
                                              left.name() + " and " + right.name()));
    }
    return Type::boolean();
  }

  Type TypeChecker::checkAdditiveBinaryExpression(const ast::BinaryExpression& binary,
                                                  const Type& left, const Type& right) const {
    if (binary.op == ast::BinaryOperator::Add && left == Type::str() && right == Type::str()) {
      return Type::str();
    }
    if (binary.op == ast::BinaryOperator::Add && (left == Type::str() || right == Type::str())) {
      throw CompileError(formatDiagnostic(binary.location, DiagnosticStage::TypeCheck,
                                          "string concatenation requires str operands, got " +
                                              left.name() + " and " + right.name()));
    }
    if (left == Type::f64() && right == Type::f64()) {
      return Type::f64();
    }
    if (left == Type::i32() && right == Type::i32()) {
      return Type::i32();
    }
    throw CompileError(
        formatDiagnostic(binary.location, DiagnosticStage::TypeCheck,
                         "arithmetic operator requires matching numeric operands, got " +
                             left.name() + " and " + right.name()));
  }

  Type TypeChecker::checkIntegerBinaryExpression(const ast::BinaryExpression& binary,
                                                 const Type& left, const Type& right) const {
    if (left != Type::i32() || right != Type::i32()) {
      throw CompileError(formatDiagnostic(binary.location, DiagnosticStage::TypeCheck,
                                          "integer operator requires i32 operands, got " +
                                              left.name() + " and " + right.name()));
    }
    return Type::i32();
  }

  Type TypeChecker::checkOrderedComparisonExpression(const ast::BinaryExpression& binary,
                                                     const Type& left, const Type& right) const {
    if (left == right && (left == Type::i32() || left == Type::f64())) {
      return Type::boolean();
    }
    throw CompileError(formatDiagnostic(binary.location, DiagnosticStage::TypeCheck,
                                        "ordered comparison requires matching numeric operands, "
                                        "got " +
                                            left.name() + " and " + right.name()));
  }

  Type TypeChecker::checkEqualityExpression(const ast::BinaryExpression& binary, const Type& left,
                                            const Type& right) const {
    if (left == right && (left == Type::i32() || left == Type::f64() || left == Type::boolean() ||
                          left == Type::str())) {
      return Type::boolean();
    }
    throw CompileError(formatDiagnostic(binary.location, DiagnosticStage::TypeCheck,
                                        "equality requires matching i32, f64, bool, or str "
                                        "operands, got " +
                                            left.name() + " and " + right.name()));
  }

  void TypeChecker::ExpressionVisitor::visit(const ast::UnaryExpression& unary) {
    const Type operandType = checker_.checkRvalue(*unary.operand);
    result_ = checker_.checkUnaryExpression(unary, operandType);
  }

  Type TypeChecker::checkUnaryExpression(const ast::UnaryExpression& unary,
                                         const Type& operandType) const {
    using UnaryCheck = Type (TypeChecker::*)(const ast::UnaryExpression&, const Type&) const;
    static const std::unordered_map<UnaryTypeCheckRule, UnaryCheck, EnumHash<UnaryTypeCheckRule>>
        checks = {
            {UnaryTypeCheckRule::Numeric, &TypeChecker::checkNumericUnaryExpression},
            {UnaryTypeCheckRule::Boolean, &TypeChecker::checkBooleanUnaryExpression},
            {UnaryTypeCheckRule::Integer, &TypeChecker::checkIntegerUnaryExpression},
        };

    const UnaryOperatorInfo* info = unaryOperatorInfo(unary.op);
    if (info == nullptr) {
      throw CompileError("typecheck: internal error: unknown unary operator");
    }

    const auto check = checks.find(info->typeCheckRule);
    if (check == checks.end()) {
      throw CompileError("typecheck: internal error: unknown unary type-check rule");
    }

    return (this->*check->second)(unary, operandType);
  }

  Type TypeChecker::checkNumericUnaryExpression(const ast::UnaryExpression& unary,
                                                const Type& operandType) const {
    if (operandType == Type::i32() || operandType == Type::f64()) {
      return operandType;
    }

    throw CompileError(
        formatDiagnostic(unary.location, DiagnosticStage::TypeCheck,
                         "unary negation requires numeric operand, got " + operandType.name()));
  }

  Type TypeChecker::checkBooleanUnaryExpression(const ast::UnaryExpression& unary,
                                                const Type& operandType) const {
    if (operandType == Type::boolean()) {
      return Type::boolean();
    }

    throw CompileError(formatDiagnostic(unary.location, DiagnosticStage::TypeCheck,
                                        "logical not requires bool operand, got " +
                                            operandType.name()));
  }

  Type TypeChecker::checkIntegerUnaryExpression(const ast::UnaryExpression& unary,
                                                const Type& operandType) const {
    if (operandType == Type::i32()) {
      return Type::i32();
    }

    throw CompileError(formatDiagnostic(unary.location, DiagnosticStage::TypeCheck,
                                        "unary operator requires i32 operand, got " +
                                            operandType.name()));
  }

  void TypeChecker::ExpressionVisitor::visit(const ast::CastExpression& castExpression) {
    const Type sourceType = checker_.checkRvalue(*castExpression.expression);
    const bool allowInternal = checker_.isStdlibContext();
    checker_.requireKnownType(castExpression.targetType, castExpression.location, nullptr, false,
                              allowInternal);
    const Type targetType = castExpression.targetType;

    if (sourceType.kind == TypeKind::RawPtr || targetType.kind == TypeKind::RawPtr) {
      throw CompileError(formatDiagnostic(castExpression.location, DiagnosticStage::TypeCheck,
                                          "cannot cast to or from __rt_ptr"));
    }

    if (sourceType == targetType) {
      result_ = targetType;
      return;
    }

    if (sourceType == Type::i32() && targetType == Type::f64()) {
      result_ = Type::f64();
      return;
    }
    if (sourceType == Type::f64() && targetType == Type::i32()) {
      result_ = Type::i32();
      return;
    }
    if (sourceType == Type::boolean() && targetType == Type::i32()) {
      result_ = Type::i32();
      return;
    }
    if (sourceType == Type::i32() && targetType == Type::boolean()) {
      result_ = Type::boolean();
      return;
    }

    throw CompileError(
        formatDiagnostic(castExpression.location, DiagnosticStage::TypeCheck,
                         "cannot cast " + sourceType.name() + " to " + targetType.name()));
  }

  void TypeChecker::ExpressionVisitor::visit(const ast::CallExpression& call) {
    if (const BuiltinSignature* descriptor = lookupBuiltin(call.callee)) {
      result_ = checker_.checkBuiltinCall(call, *descriptor);
      return;
    }

    checker_.requireFunctionCallable(call.callee, call.location);

    const auto concreteFunction = checker_.functions_.find(call.callee);
    if (concreteFunction != checker_.functions_.end()) {
      result_ = checker_.checkConcreteFunctionCall(call, concreteFunction->second);
      return;
    }

    const auto genericFunction = checker_.genericFunctions_.find(call.callee);
    if (genericFunction == checker_.genericFunctions_.end()) {
      throw CompileError(formatDiagnostic(call.location, DiagnosticStage::TypeCheck,
                                          "unknown function '" + call.callee + "'"));
    }

    result_ = checker_.checkGenericFunctionCall(call, genericFunction->second, expectedType_);
  }

  Type TypeChecker::checkConcreteFunctionCall(const ast::CallExpression& call,
                                              const FunctionSignature& signature) {
    if (call.arguments.size() != signature.parameterTypes.size()) {
      std::ostringstream out;
      out << "function '" << call.callee << "' expects " << signature.parameterTypes.size()
          << " argument(s), got " << call.arguments.size();
      throw CompileError(formatDiagnostic(call.location, DiagnosticStage::TypeCheck, out.str()));
    }

    for (std::size_t index{}; index < call.arguments.size(); ++index) {
      const Type actual = checkRvalue(*call.arguments[index]);
      const Type expected = signature.parameterTypes[index];
      if (!isAssignable(expected, actual)) {
        std::ostringstream out;
        out << "argument " << (index + 1) << " of '" << call.callee << "' expects "
            << expected.name() << ", got " << actual.name();
        throw CompileError(formatDiagnostic(call.arguments[index]->location,
                                            DiagnosticStage::TypeCheck, out.str()));
      }
    }

    return signature.returnType;
  }

  Type TypeChecker::checkGenericFunctionCall(const ast::CallExpression& call,
                                             const std::vector<std::size_t>& family,
                                             const std::optional<Type>& expectedType) {
    const bool calleeHasImplTags =
        std::any_of(family.begin(), family.end(), [&](std::size_t candidateIndex) {
          return genericFunctionAt(candidateIndex).implTag.has_value();
        });
    const bool specializedNestedImplCall =
        calleeHasImplTags && enclosingFunctionSpecializationTypeArgs() != nullptr;

    const ast::Function& signature = genericFunctionAt(family.front());
    std::unordered_map<std::string, Type> bindings;
    const std::vector<Type> typeArgs =
        inferGenericCallTypeArgs(call, signature, specializedNestedImplCall, expectedType, bindings);

    if (activeModule_ == nullptr) {
      throw CompileError("typecheck: internal error: generic function lookup without active module");
    }
    const ast::Function* selected =
        selectGenericImplementation(*activeModule_, family, findImplTag(typeArgs), call.callee,
                                    call.location);

    Substitution substitution;
    for (const auto& typeParam : selected->typeParams) {
      substitution.emplace(typeParam.name, bindings.at(typeParam.name));
    }

    checkSpecializationConstraints(call.callee, typeArgs, call.location);
    specializationRequests_.push_back(
        SpecializationRequest{call.callee, typeArgs, call.location, currentFunctionName_});
    return substitute(selected->returnType, substitution);
  }

  std::vector<Type> TypeChecker::inferGenericCallTypeArgs(
      const ast::CallExpression& call, const ast::Function& signature,
      bool seedFromSpecializedCaller, const std::optional<Type>& expectedType,
      std::unordered_map<std::string, Type>& bindings) {
    if (call.arguments.size() != signature.parameters.size()) {
      std::ostringstream out;
      out << "function '" << call.callee << "' expects " << signature.parameters.size()
          << " argument(s), got " << call.arguments.size();
      throw CompileError(formatDiagnostic(call.location, DiagnosticStage::TypeCheck, out.str()));
    }

    if (seedFromSpecializedCaller) {
      seedMatchingTypeParamsFromCaller(bindings, signature.typeParams);
    }
    for (std::size_t index{}; index < call.arguments.size(); ++index) {
      const Type actual = checkRvalue(*call.arguments[index]);
      Type expectedParam = signature.parameters[index].type;
      if (seedFromSpecializedCaller) {
        Substitution substitution(bindings.begin(), bindings.end());
        if (allTypeParamsSubstituted(expectedParam, substitution)) {
          expectedParam = substituteSpecializationType(expectedParam, substitution);
        }
      }
      unifyTypes(expectedParam, actual, bindings, call.arguments[index]->location);
    }

    seedMatchingTypeParamsFromCaller(bindings, signature.typeParams);
    seedUnboundTypeParamsFromExpectedType(bindings, signature.returnType, expectedType,
                                          call.location);
    seedUnboundTypeParamsFromCaller(bindings, signature.typeParams);

    std::vector<Type> typeArgs;
    typeArgs.reserve(signature.typeParams.size());
    for (const auto& typeParam : signature.typeParams) {
      const auto bound = bindings.find(typeParam.name);
      if (bound == bindings.end()) {
        throw CompileError(
            formatDiagnostic(call.location, DiagnosticStage::TypeCheck,
                             "cannot infer type parameter '" + typeParam.name + "'"));
      }
      typeArgs.push_back(bound->second);
    }

    return typeArgs;
  }

  void TypeChecker::ExpressionVisitor::visit(const ast::ArrayLiteral& literal) {
    if (literal.elements.empty()) {
      throw CompileError(formatDiagnostic(literal.location, DiagnosticStage::TypeCheck,
                                          "cannot infer element type of empty array literal"));
    }

    const Type elementType = checker_.checkRvalue(*literal.elements[0]);
    for (std::size_t index = 1; index < literal.elements.size(); ++index) {
      const Type actual = checker_.checkRvalue(*literal.elements[index]);
      if (actual != elementType) {
        std::ostringstream out;
        out << "array literal element " << (index + 1) << " has type " << actual.name()
            << ", expected " << elementType.name();
        throw CompileError(formatDiagnostic(literal.elements[index]->location,
                                            DiagnosticStage::TypeCheck, out.str()));
      }
    }

    rejectStructArrayElement(elementType, literal.location);
    result_ = Type::array(elementType);
  }

  void TypeChecker::ExpressionVisitor::visit(const ast::IndexExpression& index) {
    const Type baseType = checker_.checkRvalue(*index.base);
    const Type indexType = checker_.checkRvalue(*index.index);

    if (baseType.kind == TypeKind::Array) {
      if (!baseType.element) {
        throw CompileError(
            formatDiagnostic(index.base->location, DiagnosticStage::TypeCheck,
                             "index requires str or array base, got " + baseType.name()));
      }
      if (indexType != Type::i32()) {
        throw CompileError(formatDiagnostic(index.index->location, DiagnosticStage::TypeCheck,
                                            "index requires i32 index, got " + indexType.name()));
      }
      result_ = *baseType.element;
      return;
    }

    if (baseType == Type::str()) {
      if (indexType != Type::i32()) {
        throw CompileError(formatDiagnostic(index.index->location, DiagnosticStage::TypeCheck,
                                            "index requires i32 index, got " + indexType.name()));
      }
      result_ = Type::i32();
      return;
    }

    throw CompileError(
        formatDiagnostic(index.base->location, DiagnosticStage::TypeCheck,
                         "index requires str or array base, got " + baseType.name()));
  }

  void TypeChecker::ExpressionVisitor::visit(const ast::StructLiteral& literal) {
    result_ = checker_.checkStructLiteral(literal);
  }

  Type TypeChecker::checkStructLiteral(const ast::StructLiteral& literal) {
    const auto genericStruct = genericStructs_.find(literal.structName);
    if (genericStruct != genericStructs_.end()) {
      return checkGenericStructLiteral(literal, genericStructAt(genericStruct->second));
    }

    if (!literal.typeArgs.empty()) {
      throw CompileError(formatDiagnostic(literal.location, DiagnosticStage::TypeCheck,
                                          "type '" + literal.structName +
                                              "' is not generic and cannot take type arguments"));
    }

    const StructInfo& structInfo = lookupStruct(literal.structName, literal.location);
    return checkConcreteStructLiteral(literal, structInfo, {});
  }

  Type TypeChecker::checkGenericStructLiteral(const ast::StructLiteral& literal,
                                              const ast::StructDecl& templated) {
    std::vector<Type> typeArgs = literal.typeArgs;
    if (typeArgs.empty()) {
      typeArgs = inferStructLiteralTypeArgs(literal, templated);
    } else if (typeArgs.size() != templated.typeParams.size()) {
      std::ostringstream out;
      out << "struct '" << literal.structName << "' expects " << templated.typeParams.size()
          << " type argument(s), got " << typeArgs.size();
      throw CompileError(formatDiagnostic(literal.location, DiagnosticStage::TypeCheck, out.str()));
    }

    for (const Type& typeArg : typeArgs) {
      requireKnownType(typeArg, literal.location, nullptr, true);
    }

    recordStructSpecialization(literal.structName, typeArgs, literal.location);
    const StructInfo structInfo =
        resolveStructInfo(Type::structType(literal.structName, typeArgs), literal.location);
    return checkConcreteStructLiteral(literal, structInfo, typeArgs);
  }

  std::vector<Type> TypeChecker::inferStructLiteralTypeArgs(const ast::StructLiteral& literal,
                                                            const ast::StructDecl& templated) {
    std::unordered_map<std::string, Type> bindings;
    std::unordered_map<std::string, Type> provided;

    // Infer generic struct arguments from provided field values before resolving concrete fields.
    for (const auto& field : literal.fields) {
      if (provided.contains(field.name)) {
        throw CompileError(formatDiagnostic(field.location, DiagnosticStage::TypeCheck,
                                            "duplicate field '" + field.name +
                                                "' in struct literal for '" + literal.structName +
                                                "'"));
      }

      const auto templateField = std::find_if(
          templated.fields.begin(), templated.fields.end(),
          [&](const ast::StructField& candidate) { return candidate.name == field.name; });
      if (templateField == templated.fields.end()) {
        throw CompileError(formatDiagnostic(field.location, DiagnosticStage::TypeCheck,
                                            "struct '" + literal.structName + "' has no field '" +
                                                field.name + "'"));
      }

      requireFieldVisible(literal.structName,
                          StructFieldInfo{templateField->name, templateField->type, 0,
                                          templateField->visibility},
                          field.location);

      const Type actual = checkRvalue(*field.value);
      unifyTypes(templateField->type, actual, bindings, field.location);
      provided.emplace(field.name, actual);
    }

    for (const auto& expectedField : templated.fields) {
      if (!provided.contains(expectedField.name)) {
        throw CompileError(formatDiagnostic(literal.location, DiagnosticStage::TypeCheck,
                                            "struct literal for '" + literal.structName +
                                                "' is missing field '" + expectedField.name + "'"));
      }
    }

    std::vector<Type> typeArgs;
    typeArgs.reserve(templated.typeParams.size());
    for (const auto& typeParam : templated.typeParams) {
      const auto bound = bindings.find(typeParam.name);
      if (bound == bindings.end()) {
        throw CompileError(
            formatDiagnostic(literal.location, DiagnosticStage::TypeCheck,
                             "cannot infer type parameter '" + typeParam.name + "'"));
      }
      typeArgs.push_back(bound->second);
    }
    return typeArgs;
  }

  Type TypeChecker::checkConcreteStructLiteral(const ast::StructLiteral& literal,
                                               const StructInfo& structInfo,
                                               std::vector<Type> typeArgs) {
    const std::unordered_map<std::string, Type> provided =
        checkStructLiteralFields(literal, structInfo);
    requireStructLiteralComplete(literal, structInfo, provided);
    return Type::structType(literal.structName, std::move(typeArgs));
  }

  std::unordered_map<std::string, Type>
  TypeChecker::checkStructLiteralFields(const ast::StructLiteral& literal,
                                        const StructInfo& structInfo) {
    std::unordered_map<std::string, Type> provided;
    for (const auto& field : literal.fields) {
      if (provided.contains(field.name)) {
        throw CompileError(formatDiagnostic(field.location, DiagnosticStage::TypeCheck,
                                            "duplicate field '" + field.name +
                                                "' in struct literal for '" + literal.structName +
                                                "'"));
      }

      if (!structInfo.fieldIndex.contains(field.name)) {
        throw CompileError(formatDiagnostic(field.location, DiagnosticStage::TypeCheck,
                                            "struct '" + literal.structName + "' has no field '" +
                                                field.name + "'"));
      }

      const StructFieldInfo& fieldInfo = structInfo.fields.at(structInfo.fieldIndex.at(field.name));
      requireFieldVisible(literal.structName, fieldInfo, field.location);
      provided.emplace(field.name, checkRvalue(*field.value));
    }
    return provided;
  }

  void TypeChecker::requireStructLiteralComplete(
      const ast::StructLiteral& literal, const StructInfo& structInfo,
      const std::unordered_map<std::string, Type>& provided) const {
    for (const auto& expectedField : structInfo.fields) {
      const auto actual = provided.find(expectedField.name);
      if (actual == provided.end()) {
        throw CompileError(formatDiagnostic(literal.location, DiagnosticStage::TypeCheck,
                                            "struct literal for '" + literal.structName +
                                                "' is missing field '" + expectedField.name + "'"));
      }

      if (!isAssignable(expectedField.type, actual->second)) {
        throw CompileError(formatDiagnostic(
            literal.location, DiagnosticStage::TypeCheck,
            "field '" + expectedField.name + "' of '" + literal.structName + "' expects " +
                expectedField.type.name() + ", got " + actual->second.name()));
      }
    }
  }

  void TypeChecker::ExpressionVisitor::visit(const ast::FieldAccessExpression& access) {
    const Type baseType = checker_.checkRvalue(*access.base);
    if (baseType.kind != TypeKind::Struct) {
      throw CompileError(
          formatDiagnostic(access.base->location, DiagnosticStage::TypeCheck,
                           "field access requires struct base, got " + baseType.name()));
    }

    const StructInfo structInfo = checker_.resolveStructInfo(baseType, access.location);
    const auto fieldIndex = structInfo.fieldIndex.find(access.fieldName);
    if (fieldIndex == structInfo.fieldIndex.end()) {
      throw CompileError(formatDiagnostic(access.location, DiagnosticStage::TypeCheck,
                                          "struct '" + baseType.structName + "' has no field '" +
                                              access.fieldName + "'"));
    }

    const StructFieldInfo& fieldInfo = structInfo.fields[fieldIndex->second];
    checker_.requireFieldVisible(baseType.structName, fieldInfo, access.location);

    result_ = fieldInfo.type;
  }

  void TypeChecker::ExpressionVisitor::visit(const ast::ReturnStatement&) {
    unsupportedStatementInExpressionVisitor();
  }
  void TypeChecker::ExpressionVisitor::visit(const ast::LetStatement&) {
    unsupportedStatementInExpressionVisitor();
  }
  void TypeChecker::ExpressionVisitor::visit(const ast::IfStatement&) {
    unsupportedStatementInExpressionVisitor();
  }
  void TypeChecker::ExpressionVisitor::visit(const ast::WhileStatement&) {
    unsupportedStatementInExpressionVisitor();
  }
  void TypeChecker::ExpressionVisitor::visit(const ast::AssignmentStatement&) {
    unsupportedStatementInExpressionVisitor();
  }
  void TypeChecker::ExpressionVisitor::visit(const ast::ExpressionStatement&) {
    unsupportedStatementInExpressionVisitor();
  }

  void TypeChecker::CallExpressionProbe::visit(const ast::CallExpression&) {
    isCallExpression_ = true;
  }

  void TypeChecker::CallExpressionProbe::visit(const ast::IntegerLiteral&) {}
  void TypeChecker::CallExpressionProbe::visit(const ast::FloatLiteral&) {}
  void TypeChecker::CallExpressionProbe::visit(const ast::StringLiteral&) {}
  void TypeChecker::CallExpressionProbe::visit(const ast::BoolLiteral&) {}
  void TypeChecker::CallExpressionProbe::visit(const ast::UnaryExpression&) {}
  void TypeChecker::CallExpressionProbe::visit(const ast::CastExpression&) {}
  void TypeChecker::CallExpressionProbe::visit(const ast::BinaryExpression&) {}
  void TypeChecker::CallExpressionProbe::visit(const ast::IdentifierExpression&) {}
  void TypeChecker::CallExpressionProbe::visit(const ast::ArrayLiteral&) {}
  void TypeChecker::CallExpressionProbe::visit(const ast::IndexExpression&) {}
  void TypeChecker::CallExpressionProbe::visit(const ast::StructLiteral&) {}
  void TypeChecker::CallExpressionProbe::visit(const ast::FieldAccessExpression&) {}

  void TypeChecker::CallExpressionProbe::visit(const ast::ReturnStatement&) {}
  void TypeChecker::CallExpressionProbe::visit(const ast::LetStatement&) {}
  void TypeChecker::CallExpressionProbe::visit(const ast::IfStatement&) {}
  void TypeChecker::CallExpressionProbe::visit(const ast::WhileStatement&) {}
  void TypeChecker::CallExpressionProbe::visit(const ast::AssignmentStatement&) {}
  void TypeChecker::CallExpressionProbe::visit(const ast::ExpressionStatement&) {}

  namespace {

    [[noreturn]] void invalidAssignmentTarget(SourceLocation location) {
      throw CompileError(
          formatDiagnostic(location, DiagnosticStage::TypeCheck, "invalid assignment target"));
    }

    bool isFieldAssignmentPlaceBase(const ast::Expression& expression) {
      if (dynamic_cast<const ast::IdentifierExpression*>(&expression) != nullptr) {
        return true;
      }

      if (const auto* index = dynamic_cast<const ast::IndexExpression*>(&expression)) {
        const ast::Expression* root = index->base.get();
        while (const auto* nested = dynamic_cast<const ast::IndexExpression*>(root)) {
          root = nested->base.get();
        }
        return dynamic_cast<const ast::IdentifierExpression*>(root) != nullptr;
      }

      if (const auto* fieldAccess = dynamic_cast<const ast::FieldAccessExpression*>(&expression)) {
        return isFieldAssignmentPlaceBase(*fieldAccess->base);
      }

      return false;
    }

    std::string fieldAssignmentRootName(const ast::Expression& expression) {
      const ast::Expression* current = &expression;
      while (const auto* index = dynamic_cast<const ast::IndexExpression*>(current)) {
        current = index->base.get();
      }
      while (const auto* fieldAccess = dynamic_cast<const ast::FieldAccessExpression*>(current)) {
        current = fieldAccess->base.get();
      }

      if (const auto* identifier = dynamic_cast<const ast::IdentifierExpression*>(current)) {
        return identifier->name;
      }

      throw CompileError("typecheck: internal error: missing field assignment root identifier");
    }

  } // namespace

  TypeChecker::PlaceVisitor::PlaceVisitor(TypeChecker& checker) : checker_(checker) {}

  void TypeChecker::PlaceVisitor::visit(const ast::IdentifierExpression& identifier) {
    name_ = identifier.name;
    type_ = checker_.lookupLocal(identifier.name, identifier.location);
  }

  void TypeChecker::PlaceVisitor::visit(const ast::IntegerLiteral& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::PlaceVisitor::visit(const ast::FloatLiteral& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::PlaceVisitor::visit(const ast::StringLiteral& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::PlaceVisitor::visit(const ast::BoolLiteral& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::PlaceVisitor::visit(const ast::UnaryExpression& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::PlaceVisitor::visit(const ast::CastExpression& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::PlaceVisitor::visit(const ast::BinaryExpression& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::PlaceVisitor::visit(const ast::CallExpression& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::PlaceVisitor::visit(const ast::ArrayLiteral& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::PlaceVisitor::visit(const ast::StructLiteral& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::PlaceVisitor::visit(const ast::IndexExpression& index) {
    const Type baseType = checker_.checkRvalue(*index.base);
    const Type indexType = checker_.checkRvalue(*index.index);

    if (baseType == Type::str()) {
      throw CompileError(formatDiagnostic(index.location, DiagnosticStage::TypeCheck,
                                          "str index is not assignable"));
    }

    if (baseType.kind == TypeKind::Array) {
      if (!baseType.element) {
        throw CompileError(
            formatDiagnostic(index.base->location, DiagnosticStage::TypeCheck,
                             "index requires str or array base, got " + baseType.name()));
      }
      if (indexType != Type::i32()) {
        throw CompileError(formatDiagnostic(index.index->location, DiagnosticStage::TypeCheck,
                                            "index requires i32 index, got " + indexType.name()));
      }
      type_ = *baseType.element;

      const ast::Expression* root = index.base.get();
      while (const auto* nested = dynamic_cast<const ast::IndexExpression*>(root)) {
        root = nested->base.get();
      }
      if (const auto* identifier = dynamic_cast<const ast::IdentifierExpression*>(root)) {
        name_ = identifier->name;
        return;
      }
      invalidAssignmentTarget(index.location);
    }

    throw CompileError(
        formatDiagnostic(index.base->location, DiagnosticStage::TypeCheck,
                         "index requires str or array base, got " + baseType.name()));
  }

  void TypeChecker::PlaceVisitor::visit(const ast::FieldAccessExpression& access) {
    const Type baseType = checker_.checkRvalue(*access.base);
    if (baseType.kind != TypeKind::Struct) {
      throw CompileError(
          formatDiagnostic(access.base->location, DiagnosticStage::TypeCheck,
                           "field access requires struct base, got " + baseType.name()));
    }

    const StructInfo structInfo = checker_.resolveStructInfo(baseType, access.location);
    const auto fieldIndex = structInfo.fieldIndex.find(access.fieldName);
    if (fieldIndex == structInfo.fieldIndex.end()) {
      throw CompileError(formatDiagnostic(access.location, DiagnosticStage::TypeCheck,
                                          "struct '" + baseType.structName + "' has no field '" +
                                              access.fieldName + "'"));
    }

    const StructFieldInfo& fieldInfo = structInfo.fields[fieldIndex->second];
    checker_.requireFieldVisible(baseType.structName, fieldInfo, access.location);

    if (!isFieldAssignmentPlaceBase(*access.base)) {
      invalidAssignmentTarget(access.location);
    }

    name_ = fieldAssignmentRootName(*access.base) + "." + access.fieldName;
    type_ = fieldInfo.type;
  }

  void TypeChecker::PlaceVisitor::visit(const ast::ReturnStatement& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::PlaceVisitor::visit(const ast::LetStatement& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::PlaceVisitor::visit(const ast::IfStatement& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::PlaceVisitor::visit(const ast::WhileStatement& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::PlaceVisitor::visit(const ast::AssignmentStatement& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::PlaceVisitor::visit(const ast::ExpressionStatement& node) {
    invalidAssignmentTarget(node.location);
  }

  TypeChecker::PlaceInfo TypeChecker::checkPlace(const ast::Expression& place) {
    PlaceVisitor visitor(*this);
    place.accept(visitor);
    return PlaceInfo{visitor.name(), visitor.type()};
  }

  void TypeChecker::requireKnownType(const Type& type, SourceLocation location,
                                     const std::unordered_set<std::string>* allowedTypeParams,
                                     bool allowImplTags, bool allowInternalTypes) const {
    if (type == Type::i32() || type == Type::f64() || type == Type::boolean() ||
        type == Type::str())
      return;

    if (type.kind == TypeKind::RawPtr) {
      requireRawPtrUsable(type, location, allowInternalTypes);
      return;
    }

    if (type.kind == TypeKind::ImplTag) {
      requireImplTagUsable(type, location, allowImplTags);
      return;
    }

    if (type.kind == TypeKind::TypeParam) {
      requireTypeParamKnown(type, location, allowedTypeParams);
      return;
    }

    if (type.kind == TypeKind::Array) {
      requireArrayTypeKnown(type, location, allowedTypeParams, allowImplTags, allowInternalTypes);
      return;
    }

    if (type.kind == TypeKind::Struct) {
      requireStructTypeKnown(type, location, allowedTypeParams, allowInternalTypes);
      return;
    }

    throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                        "unknown type '" + type.name() + "'"));
  }

  void TypeChecker::requireRawPtrUsable(const Type&, SourceLocation location,
                                        bool allowInternalTypes) const {
    if (!allowInternalTypes) {
      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                          "__rt_ptr cannot be used outside the standard library"));
    }
  }

  void TypeChecker::requireImplTagUsable(const Type& type, SourceLocation location,
                                         bool allowImplTags) const {
    if (allowImplTags) {
      return;
    }
    throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                        "implementation tag '" +
                                            std::string(implementationTagName(type.implTag)) +
                                            "' cannot be used as a type"));
  }

  void TypeChecker::requireTypeParamKnown(
      const Type& type, SourceLocation location,
      const std::unordered_set<std::string>* allowedTypeParams) const {
    if (allowedTypeParams != nullptr && allowedTypeParams->contains(type.typeParamName)) {
      return;
    }
    throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                        "unresolved type parameter '" + type.typeParamName + "'"));
  }

  void TypeChecker::requireArrayTypeKnown(
      const Type& type, SourceLocation location,
      const std::unordered_set<std::string>* allowedTypeParams, bool allowImplTags,
      bool allowInternalTypes) const {
    if (!type.element) {
      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                          "unknown type '" + type.name() + "'"));
    }
    requireKnownType(*type.element, location, allowedTypeParams, allowImplTags,
                     allowInternalTypes);
    rejectStructArrayElement(*type.element, location);
  }

  void TypeChecker::requireStructTypeKnown(
      const Type& type, SourceLocation location,
      const std::unordered_set<std::string>* allowedTypeParams, bool allowInternalTypes) const {
    if (!type.typeArgs.empty()) {
      const auto genericStruct = genericStructs_.find(type.structName);
      if (genericStruct == genericStructs_.end()) {
        if (structs_.contains(type.structName)) {
          throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                              "type '" + type.name() +
                                                  "' is not generic and cannot take type "
                                                  "arguments"));
        }
        throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                            "unknown type '" + type.name() + "'"));
      }

      const ast::StructDecl& templated = genericStructAt(genericStruct->second);
      if (type.typeArgs.size() != templated.typeParams.size()) {
        std::ostringstream out;
        out << "type '" << type.name() << "' expects " << templated.typeParams.size()
            << " type argument(s), got " << type.typeArgs.size();
        throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck, out.str()));
      }

      for (const Type& typeArg : type.typeArgs) {
        requireKnownType(typeArg, location, allowedTypeParams, true, allowInternalTypes);
      }

      if (!containsUnboundTypeParam(type)) {
        recordStructSpecialization(type.structName, type.typeArgs, location);
      }
      return;
    }

    if (!structs_.contains(type.structName)) {
      if (genericStructs_.contains(type.structName)) {
        throw CompileError(
            formatDiagnostic(location, DiagnosticStage::TypeCheck,
                             "generic struct '" + type.structName + "' requires type arguments"));
      }
      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                          "unknown type '" + type.name() + "'"));
    }
  }

  void TypeChecker::unifyTypes(const Type& expected, const Type& actual,
                               std::unordered_map<std::string, Type>& bindings,
                               SourceLocation location) const {
    if (expected.kind == TypeKind::TypeParam) {
      bindTypeParam(expected, actual, bindings, location);
      return;
    }

    if (expected.kind == TypeKind::Array) {
      unifyArrayTypes(expected, actual, bindings, location);
      return;
    }

    if (expected.kind == TypeKind::ImplTag) {
      unifyImplTagTypes(expected, actual, location);
      return;
    }

    if (expected.kind == TypeKind::Struct) {
      unifyStructTypes(expected, actual, bindings, location);
      return;
    }

    if (!isAssignable(expected, actual)) {
      throw CompileError(
          formatDiagnostic(location, DiagnosticStage::TypeCheck,
                           "expected " + expected.name() + ", got " + actual.name()));
    }
  }

  void TypeChecker::bindTypeParam(const Type& expected, const Type& actual,
                                  std::unordered_map<std::string, Type>& bindings,
                                  SourceLocation location) const {
    const auto existing = bindings.find(expected.typeParamName);
    if (existing == bindings.end()) {
      bindings.emplace(expected.typeParamName, actual);
      return;
    }

    if (existing->second != actual) {
      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                          "conflicting types " + existing->second.name() + " and " +
                                              actual.name() + " for type parameter '" +
                                              expected.typeParamName + "'"));
    }
  }

  void TypeChecker::unifyArrayTypes(const Type& expected, const Type& actual,
                                    std::unordered_map<std::string, Type>& bindings,
                                    SourceLocation location) const {
    if (actual.kind != TypeKind::Array || !expected.element || !actual.element) {
      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                          "expected " + expected.name() + ", got " +
                                              actual.name()));
    }
    unifyTypes(*expected.element, *actual.element, bindings, location);
  }

  void TypeChecker::unifyImplTagTypes(const Type& expected, const Type& actual,
                                      SourceLocation location) const {
    if (actual.kind != TypeKind::ImplTag || expected.implTag != actual.implTag) {
      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                          "expected " + expected.name() + ", got " +
                                              actual.name()));
    }
  }

  void TypeChecker::unifyStructTypes(const Type& expected, const Type& actual,
                                     std::unordered_map<std::string, Type>& bindings,
                                     SourceLocation location) const {
    if (actual.kind != TypeKind::Struct || expected.structName != actual.structName) {
      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                          "expected " + expected.name() + ", got " +
                                              actual.name()));
    }

    if (expected.typeArgs.size() != actual.typeArgs.size()) {
      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                          "expected " + expected.name() + ", got " +
                                              actual.name()));
    }

    for (std::size_t index{}; index < expected.typeArgs.size(); ++index) {
      unifyTypes(expected.typeArgs[index], actual.typeArgs[index], bindings, location);
    }
  }

  void TypeChecker::checkSpecializationConstraints(const std::string& templateName,
                                                   const std::vector<Type>& typeArgs,
                                                   SourceLocation location) const {
    (void)templateName;

    std::optional<ImplementationTag> tag;
    for (const Type& typeArg : typeArgs) {
      if (typeArg.kind == TypeKind::ImplTag) {
        tag = typeArg.implTag;
        break;
      }
    }
    if (!tag) {
      return;
    }

    std::optional<Type> keyType;
    for (const Type& typeArg : typeArgs) {
      if (typeArg.kind != TypeKind::ImplTag) {
        keyType = typeArg;
        break;
      }
    }
    if (!keyType) {
      return;
    }

    for (const RequiredOperation operation : requiredOperations(*tag)) {
      if (supportsOperation(*keyType, operation)) {
        continue;
      }

      std::ostringstream out;
      out << "implementation tag '" << implementationTagName(*tag) << "' requires '"
          << operationName(operation) << "' for key type " << keyType->name();
      if (operation == RequiredOperation::Hash) {
        out << "; V2 hashes i32, bool, str";
      }
      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck, out.str()));
    }
  }

  void TypeChecker::recordStructSpecialization(const std::string& templateName,
                                               const std::vector<Type>& typeArgs,
                                               SourceLocation location) const {
    checkSpecializationConstraints(templateName, typeArgs, location);
    structSpecializationRequests_.push_back(
        StructSpecializationRequest{templateName, typeArgs, location, currentFunctionName_});
  }

  TypeChecker::StructInfo TypeChecker::resolveStructInfo(const Type& structType,
                                                         SourceLocation location) const {
    if (structType.kind != TypeKind::Struct) {
      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                          "internal: resolveStructInfo requires struct type"));
    }

    if (structType.typeArgs.empty()) {
      return lookupStruct(structType.structName, location);
    }

    const auto genericStruct = genericStructs_.find(structType.structName);
    if (genericStruct == genericStructs_.end()) {
      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                          "unknown type '" + structType.name() + "'"));
    }

    const ast::StructDecl& templated = genericStructAt(genericStruct->second);
    if (structType.typeArgs.size() != templated.typeParams.size()) {
      std::ostringstream out;
      out << "type '" << structType.name() << "' expects " << templated.typeParams.size()
          << " type argument(s), got " << structType.typeArgs.size();
      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck, out.str()));
    }

    Substitution substitution;
    for (std::size_t index{}; index < templated.typeParams.size(); ++index) {
      substitution.emplace(templated.typeParams[index].name, structType.typeArgs[index]);
    }

    StructInfo info;
    for (const auto& field : templated.fields) {
      const std::size_t fieldIndex = info.fields.size();
      const Type fieldType = substitute(field.type, substitution);
      info.fields.push_back(StructFieldInfo{field.name, fieldType, fieldIndex, field.visibility});
      info.fieldIndex.emplace(field.name, fieldIndex);
    }

    return info;
  }

  const TypeChecker::StructInfo& TypeChecker::lookupStruct(const std::string& name,
                                                           SourceLocation location) const {
    const auto structInfo = structs_.find(name);
    if (structInfo == structs_.end()) {
      throw CompileError(
          formatDiagnostic(location, DiagnosticStage::TypeCheck, "unknown type '" + name + "'"));
    }

    return structInfo->second;
  }

  void TypeChecker::checkStructAcyclic(const std::string& structName,
                                       SourceLocation location) const {
    std::vector<const std::string*> stack;
    std::unordered_set<std::string> visiting;

    const auto visitStruct = [&](const auto& visitStructRef, const std::string& name) -> void {
      if (!structs_.contains(name)) {
        return;
      }

      if (!visiting.insert(name).second)
        return;

      stack.push_back(&name);
      if (stack.size() > structs_.size()) {
        throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                            "struct '" + structName + "' has infinite size"));
      }

      const StructInfo& info = structs_.at(name);
      for (const auto& field : info.fields) {
        if (field.type.kind == TypeKind::Struct) {
          for (const std::string* seen : stack) {
            if (*seen == field.type.structName) {
              throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                                  "struct '" + structName + "' has infinite size"));
            }
          }
          visitStructRef(visitStructRef, field.type.structName);
        }
      }

      stack.pop_back();
      visiting.erase(name);
    };

    visitStruct(visitStruct, structName);
  }

  void TypeChecker::collectStructDecls(const ast::Module& module) {
    structs_.clear();
    genericStructs_.clear();

    for (std::size_t index{}; index < module.structs.size(); ++index) {
      const ast::StructDecl& decl = module.structs[index];
      if (structs_.contains(decl.name) || genericStructs_.contains(decl.name)) {
        throw CompileError(formatDiagnostic(decl.location, DiagnosticStage::TypeCheck,
                                            "duplicate struct '" + decl.name + "'"));
      }

      if (!decl.typeParams.empty()) {
        collectGenericStructDecl(decl, index);
        continue;
      }

      collectConcreteStructDecl(decl);
    }

    validateConcreteStructFieldTypes(module);
  }

  void TypeChecker::collectGenericStructDecl(const ast::StructDecl& decl,
                                             std::size_t moduleIndex) {
    std::unordered_set<std::string> allowedTypeParams;
    for (const auto& typeParam : decl.typeParams) {
      allowedTypeParams.insert(typeParam.name);
    }

    std::unordered_set<std::string> seenFields;
    for (const auto& field : decl.fields) {
      if (seenFields.contains(field.name)) {
        throw CompileError(formatDiagnostic(field.location, DiagnosticStage::TypeCheck,
                                            "duplicate field '" + field.name + "' in struct '" +
                                                decl.name + "'"));
      }
      seenFields.insert(field.name);
      requireKnownType(field.type, field.location, &allowedTypeParams, false,
                       allowsInternalStructTypes(decl));
    }

    genericStructs_.emplace(decl.name, moduleIndex);
  }

  void TypeChecker::collectConcreteStructDecl(const ast::StructDecl& decl) {
    if (structs_.contains(decl.name) || genericStructs_.contains(decl.name)) {
      throw CompileError(formatDiagnostic(decl.location, DiagnosticStage::TypeCheck,
                                          "duplicate struct '" + decl.name + "'"));
    }

    StructInfo info;
    std::unordered_set<std::string> seenFields;
    for (const auto& field : decl.fields) {
      if (seenFields.contains(field.name)) {
        throw CompileError(formatDiagnostic(field.location, DiagnosticStage::TypeCheck,
                                            "duplicate field '" + field.name + "' in struct '" +
                                                decl.name + "'"));
      }
      seenFields.insert(field.name);

      const std::size_t index = info.fields.size();
      info.fields.push_back(StructFieldInfo{field.name, field.type, index, field.visibility});
      info.fieldIndex.emplace(field.name, index);
    }

    structs_.emplace(decl.name, std::move(info));
  }

  void TypeChecker::validateConcreteStructFieldTypes(const ast::Module& module,
                                                     std::size_t firstStruct) {
    for (std::size_t index = firstStruct; index < module.structs.size(); ++index) {
      const ast::StructDecl& decl = module.structs[index];
      if (!decl.typeParams.empty()) {
        continue;
      }

      for (const auto& field : decl.fields) {
        requireKnownType(field.type, field.location, nullptr, false,
                         allowsInternalStructTypes(decl));
      }
      checkStructAcyclic(decl.name, decl.location);
    }
  }

  bool TypeChecker::allowsInternalStructTypes(const ast::StructDecl& decl) const {
    const auto origin = symbolOrigins_.structs.find(decl.name);
    return origin != symbolOrigins_.structs.end() && isStdlibOrigin(origin->second);
  }

  void TypeChecker::check(const ast::Module& module, const SymbolOrigins& symbolOrigins) {
    activeModule_ = &module;
    functions_.clear();
    genericFunctions_.clear();
    specializationRequests_.clear();
    structSpecializationRequests_.clear();
    scopes_.clear();
    currentFunctionName_.clear();
    symbolOrigins_ = symbolOrigins;

    collectStructDecls(module);
    collectFunctionSignatures(module);

    for (const auto& function : module.functions) {
      if (function.typeParams.empty()) {
        checkFunction(function);
      }
    }
  }

  void TypeChecker::checkSpecializationFrontier(const ast::Module& module,
                                                std::size_t firstNewStruct,
                                                std::size_t firstNewFunction,
                                                const SymbolOrigins& symbolOrigins) {
    activeModule_ = &module;
    symbolOrigins_ = symbolOrigins;
    specializationRequests_.clear();
    structSpecializationRequests_.clear();
    scopes_.clear();
    currentFunctionName_.clear();

    if (firstNewStruct > module.structs.size() || firstNewFunction > module.functions.size()) {
      throw CompileError("typecheck: internal error: invalid specialization frontier");
    }

    for (std::size_t index = firstNewStruct; index < module.structs.size(); ++index) {
      const ast::StructDecl& decl = module.structs[index];
      if (!decl.typeParams.empty()) {
        throw CompileError(
            "typecheck: internal error: specialization frontier contains generic struct");
      }
      collectConcreteStructDecl(decl);
    }
    validateConcreteStructFieldTypes(module, firstNewStruct);

    for (std::size_t index = firstNewFunction; index < module.functions.size(); ++index) {
      const ast::Function& function = module.functions[index];
      if (!function.typeParams.empty()) {
        throw CompileError(
            "typecheck: internal error: specialization frontier contains generic function");
      }
      collectConcreteFunctionSignature(function);
    }

    for (std::size_t index = firstNewFunction; index < module.functions.size(); ++index) {
      checkFunction(module.functions[index]);
    }
  }

  bool TypeChecker::isStdlibOrigin(const std::string& modulePath) const {
    return modulePath.rfind("std::", 0) == 0;
  }

  bool TypeChecker::isStdlibContext() const {
    const auto origin = symbolOrigins_.functions.find(currentFunctionName_);
    if (origin == symbolOrigins_.functions.end()) {
      return false;
    }
    return isStdlibOrigin(origin->second);
  }

  bool TypeChecker::isInternalModuleOrigin(const std::string& modulePath) const {
    return modulePath.rfind("std::internal::", 0) == 0;
  }

  void TypeChecker::requireFunctionCallable(const std::string& calleeName,
                                            SourceLocation location) const {
    const auto origin = symbolOrigins_.functions.find(calleeName);
    if (origin == symbolOrigins_.functions.end()) {
      return;
    }

    if (isInternalModuleOrigin(origin->second) && !isStdlibContext()) {
      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                          "function '" + calleeName + "' is internal to module '" +
                                              origin->second + "'"));
    }
  }

  std::string TypeChecker::structOriginModule(const std::string& structName) const {
    const auto origin = symbolOrigins_.structs.find(structName);
    if (origin == symbolOrigins_.structs.end()) {
      return "";
    }
    return origin->second;
  }

  std::string TypeChecker::currentModuleOrigin() const {
    const auto origin = symbolOrigins_.functions.find(currentFunctionName_);
    if (origin == symbolOrigins_.functions.end()) {
      return "";
    }
    return origin->second;
  }

  void TypeChecker::requireFieldVisible(const std::string& structName, const StructFieldInfo& field,
                                        SourceLocation location) const {
    if (field.visibility == ast::FieldVisibility::Public) {
      return;
    }

    const std::string declaringModule = structOriginModule(structName);
    const std::string useModule = currentModuleOrigin();
    if (declaringModule == useModule) {
      return;
    }

    throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                        "field '" + field.name + "' is private to module '" +
                                            declaringModule + "'"));
  }

  void TypeChecker::checkFunction(const ast::Function& function) {
    scopes_.clear();
    pushScope();

    currentFunctionName_ = function.name;

    const bool allowInternal = isStdlibContext();
    requireKnownType(function.returnType, function.location, nullptr, false, allowInternal);
    const Type expectedReturnType = function.returnType;

    for (const auto& parameter : function.parameters) {
      requireKnownType(parameter.type, parameter.location, nullptr, false, allowInternal);
      const Type parameterType = parameter.type;

      if (!declareLocal(parameter.name, parameterType)) {
        throw CompileError(formatDiagnostic(parameter.location, DiagnosticStage::TypeCheck,
                                            "duplicate parameter '" + parameter.name + "'"));
      }
    }

    checkStatements(function.body, expectedReturnType);
    popScope();
  }

  bool TypeChecker::checkStatements(const std::vector<std::unique_ptr<ast::Statement>>& statements,
                                    Type expectedReturnType) {
    bool returned = false;
    for (const auto& statement : statements) {
      returned = checkStatement(*statement, expectedReturnType) || returned;
    }

    return returned;
  }

  bool TypeChecker::checkStatement(const ast::Statement& statement, Type expectedReturnType) {
    StatementVisitor visitor(*this, expectedReturnType);
    statement.accept(visitor);
    return visitor.returned();
  }

  Type TypeChecker::checkBuiltinCall(const ast::CallExpression& call,
                                     const BuiltinSignature& descriptor) {
    requireBuiltinCallable(call, descriptor);

    if (descriptor.id == BuiltinId::Len) {
      return checkLenBuiltin(call);
    }

    if (descriptor.id == BuiltinId::RtSizeof) {
      return checkRtSizeofBuiltin(call);
    }

    if (descriptor.id == BuiltinId::RtHash) {
      return checkRtHashBuiltin(call);
    }

    if (descriptor.id == BuiltinId::RtLoad) {
      return checkRtLoadBuiltin(call, descriptor);
    }

    if (descriptor.id == BuiltinId::RtStore) {
      return checkRtStoreBuiltin(call, descriptor);
    }

    if (descriptor.style == MismatchStyle::AllArguments) {
      return checkAllArgumentsBuiltin(call, descriptor);
    }

    return checkDeclaredBuiltinArguments(call, descriptor);
  }

  void TypeChecker::requireBuiltinCallable(const ast::CallExpression& call,
                                           const BuiltinSignature& descriptor) const {
    if (descriptor.visibility == Visibility::Internal && !isStdlibContext()) {
      throw CompileError(formatDiagnostic(call.location, DiagnosticStage::TypeCheck,
                                          "internal runtime builtin '" +
                                              std::string(descriptor.name) +
                                              "' is unavailable outside the standard library"));
    }

    if (!builtinArityMatches(descriptor, call.arguments.size())) {
      throw CompileError(formatDiagnostic(call.location, DiagnosticStage::TypeCheck,
                                          formatBuiltinArityError(descriptor)));
    }
  }

  Type TypeChecker::checkLenBuiltin(const ast::CallExpression& call) {
    const Type actual = checkRvalue(*call.arguments[0]);
    if (actual == Type::str() || actual.kind == TypeKind::Array)
      return Type::i32();

    throw CompileError(formatDiagnostic(call.arguments[0]->location, DiagnosticStage::TypeCheck,
                                        "len expects str or array, got " + actual.name()));
  }

  Type TypeChecker::checkRtSizeofBuiltin(const ast::CallExpression& call) const {
    const Type witness = resolveWitnessType(call.location);
    if (!isScalarWitnessType(witness)) {
      throw CompileError(
          formatDiagnostic(call.location, DiagnosticStage::TypeCheck,
                           "__rt_sizeof requires a scalar element type, got " + witness.name()));
    }
    return Type::i32();
  }

  Type TypeChecker::checkRtHashBuiltin(const ast::CallExpression& call) {
    const Type witness = resolveWitnessType(call.location);
    if (!supportsOperation(witness, RequiredOperation::Hash)) {
      throw CompileError(formatDiagnostic(
          call.location, DiagnosticStage::TypeCheck,
          "__rt_hash requires a hashable key type (i32, bool, str), got " + witness.name()));
    }
    const Type value = checkRvalue(*call.arguments[0]);
    if (value != witness) {
      throw CompileError(
          formatDiagnostic(call.arguments[0]->location, DiagnosticStage::TypeCheck,
                           "__rt_hash expects " + witness.name() + ", got " + value.name()));
    }
    return Type::i32();
  }

  Type TypeChecker::checkRtLoadBuiltin(const ast::CallExpression& call,
                                       const BuiltinSignature& descriptor) {
    const Type pointer = checkRvalue(*call.arguments[0]);
    const Type index = checkRvalue(*call.arguments[1]);
    if (pointer != Type::rawPtr()) {
      throw CompileError(formatDiagnostic(
          call.arguments[0]->location, DiagnosticStage::TypeCheck,
          formatBuiltinPerArgumentMismatch(descriptor.name, TypeKind::RawPtr, pointer.name())));
    }
    if (index != Type::i32()) {
      throw CompileError(formatDiagnostic(
          call.arguments[1]->location, DiagnosticStage::TypeCheck,
          formatBuiltinPerArgumentMismatch(descriptor.name, TypeKind::I32, index.name())));
    }

    // Runtime load/store builtins use the enclosing specialization as their element witness.
    const Type witness = resolveWitnessType(call.location);
    if (!isScalarWitnessType(witness)) {
      throw CompileError(
          formatDiagnostic(call.location, DiagnosticStage::TypeCheck,
                           "__rt_load requires a scalar element type, got " + witness.name()));
    }
    return witness;
  }

  Type TypeChecker::checkRtStoreBuiltin(const ast::CallExpression& call,
                                        const BuiltinSignature& descriptor) {
    const Type pointer = checkRvalue(*call.arguments[0]);
    const Type index = checkRvalue(*call.arguments[1]);
    const Type value = checkRvalue(*call.arguments[2]);
    if (pointer != Type::rawPtr()) {
      throw CompileError(formatDiagnostic(
          call.arguments[0]->location, DiagnosticStage::TypeCheck,
          formatBuiltinPerArgumentMismatch(descriptor.name, TypeKind::RawPtr, pointer.name())));
    }
    if (index != Type::i32()) {
      throw CompileError(formatDiagnostic(
          call.arguments[1]->location, DiagnosticStage::TypeCheck,
          formatBuiltinPerArgumentMismatch(descriptor.name, TypeKind::I32, index.name())));
    }

    const Type witness = resolveWitnessType(call.location);
    if (!isScalarWitnessType(witness)) {
      throw CompileError(
          formatDiagnostic(call.location, DiagnosticStage::TypeCheck,
                           "__rt_store requires a scalar element type, got " + witness.name()));
    }
    if (value != witness) {
      throw CompileError(
          formatDiagnostic(call.arguments[2]->location, DiagnosticStage::TypeCheck,
                           "__rt_store expects " + witness.name() + ", got " + value.name()));
    }
    return Type::voidType();
  }

  Type TypeChecker::checkAllArgumentsBuiltin(const ast::CallExpression& call,
                                             const BuiltinSignature& descriptor) {
    const Type firstType = checkRvalue(*call.arguments[0]);
    const Type secondType = checkRvalue(*call.arguments[1]);
    const Type expected = Type(descriptor.parameters[0]);
    if (firstType != expected || secondType != expected) {
      throw CompileError(formatDiagnostic(
          call.location, DiagnosticStage::TypeCheck,
          formatBuiltinAllArgumentsMismatch(descriptor.name, descriptor.parameters[0],
                                            firstType.name(), secondType.name())));
    }
    return Type(descriptor.returnKind);
  }

  Type TypeChecker::checkDeclaredBuiltinArguments(const ast::CallExpression& call,
                                                  const BuiltinSignature& descriptor) {
    for (std::size_t index{}; index < descriptor.arity; ++index) {
      const Type actual = checkRvalue(*call.arguments[index]);
      const TypeKind expectedKind = descriptor.parameters[index];
      if (expectedKind == TypeKind::TypeParam) {
        continue;
      }
      const Type expected = Type(expectedKind);
      if (actual != expected) {
        throw CompileError(
            formatDiagnostic(call.arguments[index]->location, DiagnosticStage::TypeCheck,
                             formatBuiltinPerArgumentMismatch(
                                 descriptor.name, descriptor.parameters[index], actual.name())));
      }
    }

    if (descriptor.returnKind == TypeKind::TypeParam) {
      return resolveWitnessType(call.location);
    }

    return Type(descriptor.returnKind);
  }

  bool TypeChecker::isEnclosingFunctionSpecialized() const {
    return functionSpecializationTypeArgs_.contains(currentFunctionName_);
  }

  const std::vector<Type>* TypeChecker::enclosingFunctionSpecializationTypeArgs() const {
    const auto specialization = functionSpecializationTypeArgs_.find(currentFunctionName_);
    if (specialization == functionSpecializationTypeArgs_.end()) {
      return nullptr;
    }
    return &specialization->second;
  }

  void TypeChecker::seedMatchingTypeParamsFromCaller(
      std::unordered_map<std::string, Type>& bindings,
      const std::vector<ast::TypeParameter>& calleeTypeParams) const {
    const std::vector<Type>* callerTypeArgs = enclosingFunctionSpecializationTypeArgs();
    if (callerTypeArgs == nullptr) {
      return;
    }

    const std::size_t dollar = currentFunctionName_.find('$');
    if (dollar == std::string::npos) {
      return;
    }

    const std::string templateName = currentFunctionName_.substr(0, dollar);
    const auto family = genericFunctions_.find(templateName);
    if (family == genericFunctions_.end() || family->second.empty()) {
      return;
    }

    const std::vector<ast::TypeParameter>& callerTypeParams =
        genericFunctionAt(family->second.front()).typeParams;
    std::unordered_map<std::string, Type> callerBindings;
    for (std::size_t index{}; index < callerTypeParams.size() && index < callerTypeArgs->size();
         ++index) {
      callerBindings.emplace(callerTypeParams[index].name, (*callerTypeArgs)[index]);
    }

    for (const auto& calleeParam : calleeTypeParams) {
      if (bindings.contains(calleeParam.name)) {
        continue;
      }
      const auto callerBound = callerBindings.find(calleeParam.name);
      if (callerBound != callerBindings.end()) {
        bindings.emplace(calleeParam.name, callerBound->second);
      }
    }
  }

  Type TypeChecker::resolveWitnessType(SourceLocation location) const {
    const auto specialization = functionSpecializationTypeArgs_.find(currentFunctionName_);
    if (specialization == functionSpecializationTypeArgs_.end()) {
      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                          "witness-polymorphic runtime builtin requires an "
                                          "enclosing generic specialization context"));
    }

    const std::optional<Type> witness = firstNonImplTagTypeArg(specialization->second);
    if (!witness) {
      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                          "witness-polymorphic runtime builtin requires an "
                                          "enclosing generic specialization context"));
    }

    return *witness;
  }

  void TypeChecker::seedUnboundTypeParamsFromCaller(
      std::unordered_map<std::string, Type>& bindings,
      const std::vector<ast::TypeParameter>& typeParams) const {
    const auto callerSpecialization = functionSpecializationTypeArgs_.find(currentFunctionName_);
    if (callerSpecialization == functionSpecializationTypeArgs_.end()) {
      return;
    }

    std::vector<Type> callerValueTypeArgs;
    callerValueTypeArgs.reserve(callerSpecialization->second.size());
    for (const Type& typeArg : callerSpecialization->second) {
      if (typeArg.kind != TypeKind::ImplTag) {
        callerValueTypeArgs.push_back(typeArg);
      }
    }

    std::size_t callerIndex{};
    for (const auto& typeParam : typeParams) {
      if (bindings.contains(typeParam.name)) {
        continue;
      }
      if (callerIndex >= callerValueTypeArgs.size()) {
        break;
      }
      bindings.emplace(typeParam.name, callerValueTypeArgs[callerIndex]);
      ++callerIndex;
    }
  }

  void TypeChecker::seedUnboundTypeParamsFromExpectedType(
      std::unordered_map<std::string, Type>& bindings, const Type& returnType,
      const std::optional<Type>& expectedType, SourceLocation location) const {
    if (!expectedType) {
      return;
    }

    unifyTypes(returnType, *expectedType, bindings, location);
  }

  void TypeChecker::registerFunctionSpecialization(std::string mangledName,
                                                   std::vector<Type> typeArgs) {
    functionSpecializationTypeArgs_.emplace(std::move(mangledName), std::move(typeArgs));
  }

  std::vector<SpecializationRequest> TypeChecker::takeSpecializationRequests() {
    std::vector<SpecializationRequest> requests;
    requests.swap(specializationRequests_);
    return requests;
  }

  std::vector<StructSpecializationRequest> TypeChecker::takeStructSpecializationRequests() const {
    std::vector<StructSpecializationRequest> requests;
    requests.swap(structSpecializationRequests_);
    return requests;
  }

  Type TypeChecker::checkRvalue(const ast::Expression& expression,
                                std::optional<Type> expectedType) {
    ExpressionVisitor visitor(*this, std::move(expectedType));
    expression.accept(visitor);
    return visitor.result();
  }

  bool TypeChecker::declareLocal(const std::string& name, Type type) {
    if (scopes_.empty())
      pushScope();

    auto& scope = scopes_.back();
    if (scope.contains(name))
      return false;

    scope.emplace(name, type);
    return true;
  }

  Type TypeChecker::lookupLocal(const std::string& name, SourceLocation location) const {
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
      const auto local = scope->find(name);

      if (local != scope->end())
        return local->second;
    }

    throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                        "unknown local variable '" + name + "'"));
  }

  const ast::Function& TypeChecker::genericFunctionAt(std::size_t moduleIndex) const {
    if (activeModule_ == nullptr || moduleIndex >= activeModule_->functions.size()) {
      throw CompileError("typecheck: internal error: invalid generic function index");
    }
    return activeModule_->functions[moduleIndex];
  }

  const ast::StructDecl& TypeChecker::genericStructAt(std::size_t moduleIndex) const {
    if (activeModule_ == nullptr || moduleIndex >= activeModule_->structs.size()) {
      throw CompileError("typecheck: internal error: invalid generic struct index");
    }
    return activeModule_->structs[moduleIndex];
  }

  bool TypeChecker::isAssignable(Type expected, Type actual) const {
    if (expected == actual) {
      return true;
    }
    return structSpecializationsMatch(expected, actual);
  }

  void TypeChecker::collectFunctionSignatures(const ast::Module& module) {
    for (std::size_t index{}; index < module.functions.size(); ++index) {
      const ast::Function& function = module.functions[index];
      if (!function.typeParams.empty()) {
        collectGenericFunctionSignature(function, index);
        continue;
      }

      collectConcreteFunctionSignature(function);
    }

    for (const auto& [name, family] : genericFunctions_) {
      validateGenericFunctionFamily(name, family);
    }
  }

  void TypeChecker::collectGenericFunctionSignature(const ast::Function& function,
                                                    std::size_t moduleIndex) {
    const auto existing = genericFunctions_.find(function.name);
    if (existing != genericFunctions_.end()) {
      for (std::size_t candidateIndex : existing->second) {
        const ast::Function& candidate = genericFunctionAt(candidateIndex);
        if (function.implTag && candidate.implTag && *function.implTag == *candidate.implTag) {
          throw CompileError(formatDiagnostic(
              function.location, DiagnosticStage::TypeCheck,
              "duplicate implementation '" +
                  std::string(implementationTagName(*function.implTag)) +
                  "' for generic function '" + function.name + "'"));
        }
        if (static_cast<bool>(function.implTag) != static_cast<bool>(candidate.implTag)) {
          throw CompileError(
              formatDiagnostic(function.location, DiagnosticStage::TypeCheck,
                               "generic function '" + function.name +
                                   "' mixes tagged and untagged implementations"));
        }
        if (!function.implTag && !candidate.implTag) {
          throw CompileError(formatDiagnostic(function.location, DiagnosticStage::TypeCheck,
                                              "duplicate function '" + function.name + "'"));
        }
      }
    } else if (functions_.contains(function.name)) {
      throw CompileError(formatDiagnostic(function.location, DiagnosticStage::TypeCheck,
                                          "duplicate function '" + function.name + "'"));
    }

    const bool allowInternal = allowsInternalFunctionTypes(function);
    if (function.name.rfind("__rt_", 0) == 0 && !allowInternal) {
      throw CompileError(formatDiagnostic(function.location, DiagnosticStage::TypeCheck,
                                          "name '" + function.name + "' is reserved"));
    }

    std::unordered_set<std::string> allowedTypeParams;
    for (const auto& typeParam : function.typeParams) {
      allowedTypeParams.insert(typeParam.name);
    }

    requireKnownType(function.returnType, function.location, &allowedTypeParams, false,
                     allowInternal);
    for (const auto& parameter : function.parameters) {
      requireKnownType(parameter.type, parameter.location, &allowedTypeParams, false,
                       allowInternal);
    }

    genericFunctions_[function.name].push_back(moduleIndex);
  }

  void TypeChecker::collectConcreteFunctionSignature(const ast::Function& function) {
    if (functions_.contains(function.name) || genericFunctions_.contains(function.name)) {
      throw CompileError(formatDiagnostic(function.location, DiagnosticStage::TypeCheck,
                                          "duplicate function '" + function.name + "'"));
    }

    const bool allowInternal = allowsInternalFunctionTypes(function);
    if (function.name.rfind("__rt_", 0) == 0 && !allowInternal) {
      throw CompileError(formatDiagnostic(function.location, DiagnosticStage::TypeCheck,
                                          "name '" + function.name + "' is reserved"));
    }

    FunctionSignature signature;
    requireKnownType(function.returnType, function.location, nullptr, false, allowInternal);
    signature.returnType = function.returnType;

    for (const auto& parameter : function.parameters) {
      requireKnownType(parameter.type, parameter.location, nullptr, false, allowInternal);
      signature.parameterTypes.push_back(parameter.type);
    }

    functions_.emplace(function.name, std::move(signature));
  }

  void TypeChecker::validateGenericFunctionFamily(
      std::string_view name, const std::vector<std::size_t>& family) const {
    if (family.size() <= 1) {
      return;
    }

    const ast::Function& reference = genericFunctionAt(family.front());
    for (std::size_t index = 1; index < family.size(); ++index) {
      const ast::Function& candidate = genericFunctionAt(family[index]);
      if (!sameGenericPublicApi(reference, candidate)) {
        throw CompileError(formatDiagnostic(candidate.location, DiagnosticStage::TypeCheck,
                                            "implementation signature of '" + std::string(name) +
                                                "' does not match other implementations"));
      }
    }
  }

  bool TypeChecker::allowsInternalFunctionTypes(const ast::Function& function) const {
    const auto origin = symbolOrigins_.functions.find(function.name);
    return origin != symbolOrigins_.functions.end() && isStdlibOrigin(origin->second);
  }

  void TypeChecker::pushScope() {
    scopes_.emplace_back();
  }

  void TypeChecker::popScope() {
    scopes_.pop_back();
  }

} // namespace noria
