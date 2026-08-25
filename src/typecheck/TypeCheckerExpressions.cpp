#include "TypeCheckerInternal.hpp"
#include "TypeCheckerStrategy.hpp"

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

  TypeChecker::ExpressionVisitor::ExpressionVisitor(TypeChecker& checker,
                                                    std::optional<Type> expectedType)
      : ExpressionOnlyVisitor("typecheck"), checker_(checker),
        expectedType_(std::move(expectedType)) {}

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

    using BinaryCheck =
        Type (TypeChecker::*)(const ast::BinaryExpression&, const Type&, const Type&) const;
    static const std::unordered_map<BinaryTypeCheckRule, BinaryCheck, EnumHash<BinaryTypeCheckRule>>
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

    const Type result = (this->*check->second)(binary, left, right);
    rejectStaticallyInvalidIntegerOperation(binary, result);
    return result;
  }

  void TypeChecker::rejectStaticallyInvalidIntegerOperation(const ast::BinaryExpression& binary,
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

    throw CompileError(
        formatDiagnostic(unary.location, DiagnosticStage::TypeCheck,
                         "logical not requires bool operand, got " + operandType.name()));
  }

  Type TypeChecker::checkIntegerUnaryExpression(const ast::UnaryExpression& unary,
                                                const Type& operandType) const {
    if (operandType == Type::i32()) {
      return Type::i32();
    }

    throw CompileError(
        formatDiagnostic(unary.location, DiagnosticStage::TypeCheck,
                         "unary operator requires i32 operand, got " + operandType.name()));
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

  void TypeChecker::ExpressionVisitor::visit(const ast::ArrayLiteral& literal) {
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

    const Type elementType = checker_.checkRvalue(*literal.elements[0], expectedElementType);
    for (std::size_t index = 1; index < literal.elements.size(); ++index) {
      const Type actual = checker_.checkRvalue(*literal.elements[index], expectedElementType);
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

} // namespace noria
