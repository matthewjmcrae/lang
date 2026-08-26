#include "TypeCheckerInternal.hpp"
#include "TypeCheckerState.hpp"

#include "noria/Builtins.hpp"
#include "noria/Constraints.hpp"
#include "noria/Diagnostic.hpp"
#include "noria/Monomorphize.hpp"
#include "noria/SemanticTables.hpp"

#include "TypeCheckerSupport.hpp"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace noria {

  using namespace typecheck_detail;

  TypeChecker::ExpressionsState::ExpressionVisitor::ExpressionVisitor(ExpressionsState& state,
                                                    std::optional<Type> expectedType)
      : ExpressionOnlyVisitor("typecheck"), state_(state),
        expectedType_(std::move(expectedType)) {}

  void TypeChecker::ExpressionsState::ExpressionVisitor::visit(const ast::IntegerLiteral& integer) {
    if (integer.value < std::numeric_limits<std::int32_t>::min() ||
        integer.value > std::numeric_limits<std::int32_t>::max()) {
      throw CompileError(formatDiagnostic(integer.location, DiagnosticStage::TypeCheck,
                                          "integer literal out of i32 range"));
    }
    result_ = Type::i32();
  }

  void TypeChecker::ExpressionsState::ExpressionVisitor::visit(const ast::FloatLiteral&) {
    result_ = Type::f64();
  }

  void TypeChecker::ExpressionsState::ExpressionVisitor::visit(const ast::StringLiteral&) {
    result_ = Type::str();
  }

  void TypeChecker::ExpressionsState::ExpressionVisitor::visit(const ast::BoolLiteral&) {
    result_ = Type::boolean();
  }

  void TypeChecker::ExpressionsState::ExpressionVisitor::visit(const ast::IdentifierExpression& identifier) {
    result_ = state_.lookupLocal(identifier.name, identifier.location);
  }

  void TypeChecker::ExpressionsState::ExpressionVisitor::visit(const ast::BinaryExpression& binary) {
    const Type left = state_.checkRvalue(*binary.left);
    const Type right = state_.checkRvalue(*binary.right);
    result_ = state_.checkBinaryExpression(binary, left, right);
  }

  Type TypeChecker::ExpressionsState::checkBinaryExpression(const ast::BinaryExpression& binary, const Type& left,
                                          const Type& right) const {
    if (left.kind == TypeKind::RawPtr || right.kind == TypeKind::RawPtr) {
      throw CompileError(formatDiagnostic(binary.location, DiagnosticStage::TypeCheck,
                                          "invalid operation on __rt_ptr"));
    }

    using BinaryCheck =
        Type (TypeChecker::ExpressionsState::*)(const ast::BinaryExpression&, const Type&, const Type&) const;
    static const std::unordered_map<BinaryTypeCheckRule, BinaryCheck, EnumHash<BinaryTypeCheckRule>>
        checks = {
            {BinaryTypeCheckRule::Logical, &TypeChecker::ExpressionsState::checkLogicalBinaryExpression},
            {BinaryTypeCheckRule::Numeric, &TypeChecker::ExpressionsState::checkAdditiveBinaryExpression},
            {BinaryTypeCheckRule::Integer, &TypeChecker::ExpressionsState::checkIntegerBinaryExpression},
            {BinaryTypeCheckRule::OrderedComparison,
             &TypeChecker::ExpressionsState::checkOrderedComparisonExpression},
            {BinaryTypeCheckRule::Equality, &TypeChecker::ExpressionsState::checkEqualityExpression},
        };

    const BinaryOperatorInfo* info = binaryOperatorInfo(binary.op);
    if (info == nullptr) {
      throw CompileError("typecheck: internal error: unknown binary operator");
    }

    const auto check = checks.find(info->typeCheckRule);
    if (check == checks.end()) {
      throw CompileError("typecheck: internal error: unknown binary type-check rule");
    }

    const Type result = (this->*check->second)(binary, left, right);
    rejectStaticallyInvalidIntegerOperation(binary, result);
    return result;
  }

  void TypeChecker::ExpressionsState::rejectStaticallyInvalidIntegerOperation(const ast::BinaryExpression& binary,
                                                            const Type& result) const {
    if (result != Type::i32()) {
      return;
    }

    const BinaryOperatorInfo* info = binaryOperatorInfo(binary.op);
    if (info == nullptr) {
      throw CompileError("typecheck: internal error: unknown binary operator");
    }

    const auto* right = dynamic_cast<const ast::IntegerLiteral*>(binary.right.get());
    if (right == nullptr || info->integerSafetyRule == IntegerSafetyRule::None) {
      return;
    }

    if (info->integerSafetyRule == IntegerSafetyRule::ShiftCount) {
      if (right->value < 0 || right->value >= 32) {
        throw CompileError(formatDiagnostic(binary.location, DiagnosticStage::TypeCheck,
                                            "integer shift count out of range (expected 0..31)"));
      }
      return;
    }

    if (right->value == 0) {
      const std::string message = binary.op == ast::BinaryOperator::Divide
                                      ? "integer division by zero"
                                      : "integer remainder by zero";
      throw CompileError(formatDiagnostic(binary.location, DiagnosticStage::TypeCheck, message));
    }

    const auto* left = dynamic_cast<const ast::IntegerLiteral*>(binary.left.get());
    if (left == nullptr || left->value != std::numeric_limits<std::int32_t>::min() ||
        right->value != -1) {
      return;
    }

    const std::string message = binary.op == ast::BinaryOperator::Divide
                                    ? "integer division overflow"
                                    : "integer remainder overflow";
    throw CompileError(formatDiagnostic(binary.location, DiagnosticStage::TypeCheck, message));
  }

  Type TypeChecker::ExpressionsState::checkLogicalBinaryExpression(const ast::BinaryExpression& binary,
                                                 const Type& left, const Type& right) const {
    if (left != Type::boolean() || right != Type::boolean()) {
      throw CompileError(formatDiagnostic(binary.location, DiagnosticStage::TypeCheck,
                                          "logical operator requires bool operands, got " +
                                              left.name() + " and " + right.name()));
    }
    return Type::boolean();
  }

  Type TypeChecker::ExpressionsState::checkAdditiveBinaryExpression(const ast::BinaryExpression& binary,
                                                  const Type& left, const Type& right) const {
    if (binary.op == ast::BinaryOperator::Add && left == Type::str() && right == Type::str()) {
      return Type::str();
    }
    if (binary.op == ast::BinaryOperator::Add && (left == Type::str() || right == Type::str())) {
      throw CompileError(formatDiagnostic(binary.location, DiagnosticStage::TypeCheck,
                                          "string concatenation requires str operands, got " +
                                              left.name() + " and " + right.name()));
    }
    if (binary.op == ast::BinaryOperator::Add &&
        (left.kind == TypeKind::Array || right.kind == TypeKind::Array)) {
      if (left.kind != TypeKind::Array || right.kind != TypeKind::Array || left != right) {
        throw CompileError(formatDiagnostic(binary.location, DiagnosticStage::TypeCheck,
                                            "collection addition requires matching array types, got " +
                                                left.name() + " and " + right.name()));
      }
      if (!left.element || !supportsCollectionAddition(*left.element)) {
        throw CompileError(formatDiagnostic(binary.location, DiagnosticStage::TypeCheck,
                                            "collection addition requires addable array elements, got " +
                                                (left.element ? left.element->name() : "unknown")));
      }
      return left;
    }

    const std::optional<Type> leftSequenceElement = sequenceElementType(left);
    const std::optional<Type> rightSequenceElement = sequenceElementType(right);
    if (binary.op == ast::BinaryOperator::Add && (leftSequenceElement || rightSequenceElement)) {
      if (!leftSequenceElement || !rightSequenceElement || left != right) {
        throw CompileError(formatDiagnostic(
            binary.location, DiagnosticStage::TypeCheck,
            "collection addition requires matching Sequence operands, got " + left.name() +
                " and " + right.name()));
      }
      if (!supportsCollectionAddition(*leftSequenceElement)) {
        throw CompileError(formatDiagnostic(binary.location, DiagnosticStage::TypeCheck,
                                            "collection addition requires addable Sequence elements, got " +
                                                leftSequenceElement->name()));
      }
      return left;
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

  std::optional<Type> TypeChecker::ExpressionsState::sequenceElementType(const Type& type) const {
    if (type.kind != TypeKind::Struct) {
      return std::nullopt;
    }

    std::string structName = type.structName;
    std::vector<Type> typeArgs = type.typeArgs;
    if (typeArgs.empty()) {
      const auto specialization = session().structSpecializationTypeArgs.find(structName);
      if (specialization == session().structSpecializationTypeArgs.end()) {
        return std::nullopt;
      }
      const std::size_t dollar = structName.find('$');
      if (dollar == std::string::npos) {
        return std::nullopt;
      }
      structName.erase(dollar);
      typeArgs = specialization->second;
    }

    if (standardContainerKindFromStructName(structName) != StandardContainer::Sequence ||
        typeArgs.size() != 2 || typeArgs[1].kind != TypeKind::ImplTag) {
      return std::nullopt;
    }

    const auto origin = environment().symbolOrigins.structs.find(type.structName);
    if (origin == environment().symbolOrigins.structs.end() || origin->second != "std::sequence") {
      return std::nullopt;
    }
    return typeArgs.front();
  }

  bool TypeChecker::ExpressionsState::supportsCollectionAddition(const Type& type) const {
    if (type == Type::i32() || type == Type::f64() || type == Type::str()) {
      return true;
    }
    return type.kind == TypeKind::Array && type.element && supportsCollectionAddition(*type.element);
  }

  Type TypeChecker::ExpressionsState::checkIntegerBinaryExpression(const ast::BinaryExpression& binary,
                                                 const Type& left, const Type& right) const {
    if (left != Type::i32() || right != Type::i32()) {
      throw CompileError(formatDiagnostic(binary.location, DiagnosticStage::TypeCheck,
                                          "integer operator requires i32 operands, got " +
                                              left.name() + " and " + right.name()));
    }
    return Type::i32();
  }

  Type TypeChecker::ExpressionsState::checkOrderedComparisonExpression(const ast::BinaryExpression& binary,
                                                     const Type& left, const Type& right) const {
    if (left == right && (left == Type::i32() || left == Type::f64())) {
      return Type::boolean();
    }
    throw CompileError(formatDiagnostic(binary.location, DiagnosticStage::TypeCheck,
                                        "ordered comparison requires matching numeric operands, "
                                        "got " +
                                            left.name() + " and " + right.name()));
  }

  Type TypeChecker::ExpressionsState::checkEqualityExpression(const ast::BinaryExpression& binary, const Type& left,
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

  void TypeChecker::ExpressionsState::ExpressionVisitor::visit(const ast::UnaryExpression& unary) {
    const Type operandType = state_.checkRvalue(*unary.operand);
    result_ = state_.checkUnaryExpression(unary, operandType);
  }

  Type TypeChecker::ExpressionsState::checkUnaryExpression(const ast::UnaryExpression& unary,
                                         const Type& operandType) const {
    using UnaryCheck = Type (TypeChecker::ExpressionsState::*)(const ast::UnaryExpression&, const Type&) const;
    static const std::unordered_map<UnaryTypeCheckRule, UnaryCheck, EnumHash<UnaryTypeCheckRule>>
        checks = {
            {UnaryTypeCheckRule::Numeric, &TypeChecker::ExpressionsState::checkNumericUnaryExpression},
            {UnaryTypeCheckRule::Boolean, &TypeChecker::ExpressionsState::checkBooleanUnaryExpression},
            {UnaryTypeCheckRule::Integer, &TypeChecker::ExpressionsState::checkIntegerUnaryExpression},
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

  Type TypeChecker::ExpressionsState::checkNumericUnaryExpression(const ast::UnaryExpression& unary,
                                                const Type& operandType) const {
    if (operandType == Type::i32() || operandType == Type::f64()) {
      return operandType;
    }

    throw CompileError(
        formatDiagnostic(unary.location, DiagnosticStage::TypeCheck,
                         "unary negation requires numeric operand, got " + operandType.name()));
  }

  Type TypeChecker::ExpressionsState::checkBooleanUnaryExpression(const ast::UnaryExpression& unary,
                                                const Type& operandType) const {
    if (operandType == Type::boolean()) {
      return Type::boolean();
    }

    throw CompileError(
        formatDiagnostic(unary.location, DiagnosticStage::TypeCheck,
                         "logical not requires bool operand, got " + operandType.name()));
  }

  Type TypeChecker::ExpressionsState::checkIntegerUnaryExpression(const ast::UnaryExpression& unary,
                                                const Type& operandType) const {
    if (operandType == Type::i32()) {
      return Type::i32();
    }

    throw CompileError(
        formatDiagnostic(unary.location, DiagnosticStage::TypeCheck,
                         "unary operator requires i32 operand, got " + operandType.name()));
  }

  void TypeChecker::ExpressionsState::ExpressionVisitor::visit(const ast::CastExpression& castExpression) {
    const Type sourceType = state_.checkRvalue(*castExpression.expression);
    const bool allowInternal = state_.isStdlibContext();
    state_.requireKnownType(castExpression.targetType, castExpression.location, nullptr, false,
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

  void TypeChecker::ExpressionsState::ExpressionVisitor::visit(const ast::ArrayLiteral& literal) {
    std::optional<Type> expectedElementType;
    if (expectedType_ && expectedType_->kind == TypeKind::Array && expectedType_->element &&
        !containsUnboundTypeParam(*expectedType_->element)) {
      expectedElementType = *expectedType_->element;
    }

    if (literal.elements.empty()) {
      if (expectedType_ && expectedType_->kind == TypeKind::Array && expectedType_->element &&
          !containsUnboundTypeParam(*expectedType_)) {
        rejectStructArrayElement(*expectedType_->element, literal.location);
        result_ = *expectedType_;
        return;
      }
      throw CompileError(formatDiagnostic(literal.location, DiagnosticStage::TypeCheck,
                                          "cannot infer element type of empty array literal"));
    }

    const Type elementType = state_.checkRvalue(*literal.elements[0], expectedElementType);
    for (std::size_t index = 1; index < literal.elements.size(); ++index) {
      const Type actual = state_.checkRvalue(*literal.elements[index], expectedElementType);
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

  void TypeChecker::ExpressionsState::ExpressionVisitor::visit(const ast::IndexExpression& index) {
    const Type baseType = state_.checkRvalue(*index.base);
    const Type indexType = state_.checkRvalue(*index.index);

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

    const Type canonical = state_.canonicalStructType(baseType);
    if (const std::optional<StandardContainer> container =
            state_.standardContainerFor(canonical)) {
      const auto requireIndexType = [&](const Type& expected, std::string_view name) {
        if (!state_.isAssignable(expected, indexType)) {
          throw CompileError(formatDiagnostic(index.index->location, DiagnosticStage::TypeCheck,
                                              std::string(name) + " index expects " +
                                                  expected.name() + ", got " + indexType.name()));
        }
      };

      switch (*container) {
      case StandardContainer::Sequence:
        requireIndexType(Type::i32(), "sequence");
        state_.recordImplicitContainerOperation(*container, ContainerOperation::Get,
                                                  canonical.typeArgs, index.location);
        index.standardContainer = std::make_pair(*container, canonical);
        result_ = canonical.typeArgs[0];
        return;
      case StandardContainer::Dictionary:
        requireIndexType(canonical.typeArgs[0], "dictionary");
        state_.recordImplicitContainerOperation(*container, ContainerOperation::Contains,
                                                  canonical.typeArgs, index.location);
        state_.recordImplicitContainerOperation(*container, ContainerOperation::Get,
                                                  canonical.typeArgs, index.location);
        state_.recordImplicitContainerOperation(*container, ContainerOperation::Insert,
                                                  canonical.typeArgs, index.location);
        state_.requireDefaultInitializable(canonical.typeArgs[1], index.location);
        index.standardContainer = std::make_pair(*container, canonical);
        result_ = canonical.typeArgs[1];
        return;
      case StandardContainer::Set:
        requireIndexType(canonical.typeArgs[0], "set");
        state_.recordImplicitContainerOperation(*container, ContainerOperation::Contains,
                                                  canonical.typeArgs, index.location);
        index.standardContainer = std::make_pair(*container, canonical);
        result_ = Type::boolean();
        return;
      }
    }

    throw CompileError(
        formatDiagnostic(index.base->location, DiagnosticStage::TypeCheck,
                         "index requires str, array, Sequence, Dictionary, or Set base, got " +
                             baseType.name()));
  }

  void TypeChecker::ExpressionsState::ExpressionVisitor::visit(const ast::FieldAccessExpression& access) {
    const Type baseType = state_.checkRvalue(*access.base);
    if (baseType.kind != TypeKind::Struct) {
      throw CompileError(
          formatDiagnostic(access.base->location, DiagnosticStage::TypeCheck,
                           "field access requires struct base, got " + baseType.name()));
    }

    const StructInfo structInfo = state_.resolveStructInfo(baseType, access.location);
    const auto fieldIndex = structInfo.fieldIndex.find(access.fieldName);
    if (fieldIndex == structInfo.fieldIndex.end()) {
      throw CompileError(formatDiagnostic(access.location, DiagnosticStage::TypeCheck,
                                          "struct '" + baseType.structName + "' has no field '" +
                                              access.fieldName + "'"));
    }

    const StructFieldInfo& fieldInfo = structInfo.fields[fieldIndex->second];
    state_.requireFieldVisible(baseType.structName, fieldInfo, access.location);

    result_ = fieldInfo.type;
  }

} // namespace noria
