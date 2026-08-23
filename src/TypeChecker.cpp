#include "noria/TypeChecker.hpp"

#include "noria/Builtins.hpp"
#include "noria/Diagnostic.hpp"

#include <sstream>
#include <unordered_set>

namespace noria {

  namespace {

    [[noreturn]] void unsupportedExpressionInStatementVisitor() {
      throw CompileError("typecheck: internal error: expression visited by statement visitor");
    }

    [[noreturn]] void unsupportedStatementInExpressionVisitor() {
      throw CompileError("typecheck: internal error: statement visited by expression visitor");
    }

  } // namespace

  TypeChecker::StatementVisitor::StatementVisitor(TypeChecker& checker, Type expectedReturnType)
      : checker_(checker), expectedReturnType_(expectedReturnType) {}

  void TypeChecker::StatementVisitor::visit(const ast::LetStatement& letStatement) {
    checker_.requireKnownType(letStatement.type, letStatement.location);
    const Type declaredType = letStatement.type;
    const Type initializerType = checker_.checkRvalue(*letStatement.initializer);

    if (!checker_.declareLocal(letStatement.name, declaredType)) {
      throw CompileError(formatDiagnostic(letStatement.location, DiagnosticStage::TypeCheck,
                                          "duplicate local variable '" + letStatement.name + "'"));
    }

    if (!checker_.isAssignable(declaredType, initializerType)) {
      throw CompileError(
          formatDiagnostic(letStatement.initializer->location, DiagnosticStage::TypeCheck,
                           "cannot initialize '" + letStatement.name + "' of type " +
                               declaredType.name() + " with " + initializerType.name()));
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
    const Type returnType = checker_.checkRvalue(*returnStatement.expression);

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

  TypeChecker::ExpressionVisitor::ExpressionVisitor(TypeChecker& checker) : checker_(checker) {}

  void TypeChecker::ExpressionVisitor::visit(const ast::IntegerLiteral&) {
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

    switch (binary.op) {
    case ast::BinaryOperator::And:
    case ast::BinaryOperator::Or:
      if (left != Type::boolean() || right != Type::boolean()) {
        throw CompileError(formatDiagnostic(binary.location, DiagnosticStage::TypeCheck,
                                            "logical operator requires bool operands, got " +
                                                left.name() + " and " + right.name()));
      }
      result_ = Type::boolean();
      return;
    case ast::BinaryOperator::Add:
      if (left == Type::str() && right == Type::str()) {
        result_ = Type::str();
        return;
      }
      if (left == Type::str() || right == Type::str()) {
        throw CompileError(formatDiagnostic(binary.location, DiagnosticStage::TypeCheck,
                                            "string concatenation requires str operands, got " +
                                                left.name() + " and " + right.name()));
      }
      [[fallthrough]];
    case ast::BinaryOperator::Subtract:
    case ast::BinaryOperator::Multiply:
    case ast::BinaryOperator::Divide:
      if (left == Type::f64() && right == Type::f64()) {
        result_ = Type::f64();
        return;
      }
      if (left == Type::i32() && right == Type::i32()) {
        result_ = Type::i32();
        return;
      }
      throw CompileError(
          formatDiagnostic(binary.location, DiagnosticStage::TypeCheck,
                           "arithmetic operator requires matching numeric operands, got " +
                               left.name() + " and " + right.name()));
    case ast::BinaryOperator::Modulo:
    case ast::BinaryOperator::BitAnd:
    case ast::BinaryOperator::BitOr:
    case ast::BinaryOperator::BitXor:
    case ast::BinaryOperator::Shl:
    case ast::BinaryOperator::Shr:
      if (left != Type::i32() || right != Type::i32()) {
        throw CompileError(formatDiagnostic(binary.location, DiagnosticStage::TypeCheck,
                                            "integer operator requires i32 operands, got " +
                                                left.name() + " and " + right.name()));
      }
      result_ = Type::i32();
      return;
    case ast::BinaryOperator::Less:
    case ast::BinaryOperator::LessEqual:
    case ast::BinaryOperator::Greater:
    case ast::BinaryOperator::GreaterEqual:
    case ast::BinaryOperator::Equal:
    case ast::BinaryOperator::NotEqual:
      if (left == right && (left == Type::i32() || left == Type::f64())) {
        result_ = Type::boolean();
        return;
      }
      throw CompileError(formatDiagnostic(binary.location, DiagnosticStage::TypeCheck,
                                          "comparison requires matching numeric operands, got " +
                                              left.name() + " and " + right.name()));
    }
  }

  void TypeChecker::ExpressionVisitor::visit(const ast::UnaryExpression& unary) {
    const Type operandType = checker_.checkRvalue(*unary.operand);

    switch (unary.op) {
    case ast::UnaryOperator::Negate:
      if (operandType == Type::i32() || operandType == Type::f64()) {
        result_ = operandType;
        return;
      }
      throw CompileError(
          formatDiagnostic(unary.location, DiagnosticStage::TypeCheck,
                           "unary negation requires numeric operand, got " + operandType.name()));
    case ast::UnaryOperator::BitNot:
      if (operandType != Type::i32()) {
        throw CompileError(
            formatDiagnostic(unary.location, DiagnosticStage::TypeCheck,
                             "unary operator requires i32 operand, got " + operandType.name()));
      }
      result_ = Type::i32();
      return;
    case ast::UnaryOperator::Not:
      if (operandType != Type::boolean()) {
        throw CompileError(
            formatDiagnostic(unary.location, DiagnosticStage::TypeCheck,
                             "logical not requires bool operand, got " + operandType.name()));
      }
      result_ = Type::boolean();
      return;
    }
  }

  void TypeChecker::ExpressionVisitor::visit(const ast::CastExpression& castExpression) {
    const Type sourceType = checker_.checkRvalue(*castExpression.expression);
    checker_.requireKnownType(castExpression.targetType, castExpression.location);
    const Type targetType = castExpression.targetType;

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

    const auto function = checker_.functions_.find(call.callee);

    if (function == checker_.functions_.end()) {
      throw CompileError(formatDiagnostic(call.location, DiagnosticStage::TypeCheck,
                                          "unknown function '" + call.callee + "'"));
    }

    if (call.arguments.size() != function->second.parameterTypes.size()) {
      std::ostringstream out;
      out << "function '" << call.callee << "' expects " << function->second.parameterTypes.size()
          << " argument(s), got " << call.arguments.size();
      throw CompileError(formatDiagnostic(call.location, DiagnosticStage::TypeCheck, out.str()));
    }

    for (std::size_t index{}; index < call.arguments.size(); ++index) {
      const Type actual = checker_.checkRvalue(*call.arguments[index]);
      const Type expected = function->second.parameterTypes[index];
      if (!checker_.isAssignable(expected, actual)) {
        std::ostringstream out;
        out << "argument " << (index + 1) << " of '" << call.callee << "' expects "
            << expected.name() << ", got " << actual.name();
        throw CompileError(formatDiagnostic(call.arguments[index]->location,
                                            DiagnosticStage::TypeCheck, out.str()));
      }
    }

    result_ = function->second.returnType;
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
    const StructInfo& structInfo = checker_.lookupStruct(literal.structName, literal.location);

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

      provided.emplace(field.name, checker_.checkRvalue(*field.value));
    }

    for (const auto& expectedField : structInfo.fields) {
      const auto actual = provided.find(expectedField.name);
      if (actual == provided.end()) {
        throw CompileError(formatDiagnostic(literal.location, DiagnosticStage::TypeCheck,
                                            "struct literal for '" + literal.structName +
                                                "' is missing field '" + expectedField.name + "'"));
      }

      if (!checker_.isAssignable(expectedField.type, actual->second)) {
        throw CompileError(formatDiagnostic(
            literal.location, DiagnosticStage::TypeCheck,
            "field '" + expectedField.name + "' of '" + literal.structName + "' expects " +
                expectedField.type.name() + ", got " + actual->second.name()));
      }
    }

    result_ = Type::structType(literal.structName);
  }

  void TypeChecker::ExpressionVisitor::visit(const ast::FieldAccessExpression& access) {
    const Type baseType = checker_.checkRvalue(*access.base);
    if (baseType.kind != TypeKind::Struct) {
      throw CompileError(
          formatDiagnostic(access.base->location, DiagnosticStage::TypeCheck,
                           "field access requires struct base, got " + baseType.name()));
    }

    const StructInfo& structInfo = checker_.lookupStruct(baseType.structName, access.location);
    const auto field = structInfo.fieldIndex.find(access.fieldName);
    if (field == structInfo.fieldIndex.end()) {
      throw CompileError(formatDiagnostic(access.location, DiagnosticStage::TypeCheck,
                                          "struct '" + baseType.structName + "' has no field '" +
                                              access.fieldName + "'"));
    }

    result_ = structInfo.fields[field->second].type;
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

    const StructInfo& structInfo = checker_.lookupStruct(baseType.structName, access.location);
    const auto field = structInfo.fieldIndex.find(access.fieldName);
    if (field == structInfo.fieldIndex.end()) {
      throw CompileError(formatDiagnostic(access.location, DiagnosticStage::TypeCheck,
                                          "struct '" + baseType.structName + "' has no field '" +
                                              access.fieldName + "'"));
    }

    if (!isFieldAssignmentPlaceBase(*access.base)) {
      invalidAssignmentTarget(access.location);
    }

    name_ = fieldAssignmentRootName(*access.base) + "." + access.fieldName;
    type_ = structInfo.fields[field->second].type;
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

  void TypeChecker::requireKnownType(const Type& type, SourceLocation location) const {
    if (type == Type::i32() || type == Type::f64() || type == Type::boolean() ||
        type == Type::str())
      return;

    if (type.kind == TypeKind::Array) {
      if (!type.element) {
        throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                            "unknown type '" + type.name() + "'"));
      }
      requireKnownType(*type.element, location);
      return;
    }

    if (type.kind == TypeKind::Struct) {
      if (!structs_.contains(type.structName)) {
        throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                            "unknown type '" + type.name() + "'"));
      }
      return;
    }

    throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                        "unknown type '" + type.name() + "'"));
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

    for (const auto& decl : module.structs) {
      if (structs_.contains(decl.name)) {
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
        info.fields.push_back(StructFieldInfo{field.name, field.type, index});
        info.fieldIndex.emplace(field.name, index);
      }

      structs_.emplace(decl.name, std::move(info));
    }

    for (const auto& decl : module.structs) {
      for (const auto& field : decl.fields) {
        requireKnownType(field.type, field.location);
      }
      checkStructAcyclic(decl.name, decl.location);
    }
  }

  void TypeChecker::check(const ast::Module& module) {
    functions_.clear();
    scopes_.clear();

    collectStructDecls(module);
    collectFunctionSignatures(module);

    for (const auto& function : module.functions) {
      checkFunction(function);
    }
  }

  void TypeChecker::checkFunction(const ast::Function& function) {
    scopes_.clear();
    pushScope();

    requireKnownType(function.returnType, function.location);
    const Type expectedReturnType = function.returnType;

    for (const auto& parameter : function.parameters) {
      requireKnownType(parameter.type, parameter.location);
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

    for (const auto& statement : statements) {
      if (checkStatement(*statement, expectedReturnType)) {
        return true;
      }
    }

    return false;
  }

  bool TypeChecker::checkStatement(const ast::Statement& statement, Type expectedReturnType) {
    StatementVisitor visitor(*this, expectedReturnType);
    statement.accept(visitor);
    return visitor.returned();
  }

  Type TypeChecker::checkBuiltinCall(const ast::CallExpression& call,
                                     const BuiltinSignature& descriptor) {
    if (!builtinArityMatches(descriptor, call.arguments.size())) {
      throw CompileError(formatDiagnostic(call.location, DiagnosticStage::TypeCheck,
                                          formatBuiltinArityError(descriptor)));
    }

    if (descriptor.id == BuiltinId::Len) {
      const Type actual = checkRvalue(*call.arguments[0]);
      if (actual == Type::str() || actual.kind == TypeKind::Array)
        return Type::i32();

      throw CompileError(formatDiagnostic(call.arguments[0]->location, DiagnosticStage::TypeCheck,
                                          "len expects str or array, got " + actual.name()));
    }

    if (descriptor.style == MismatchStyle::AllArguments) {
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

    for (std::size_t index{}; index < descriptor.arity; ++index) {
      const Type actual = checkRvalue(*call.arguments[index]);
      const Type expected = Type(descriptor.parameters[index]);
      if (actual != expected) {
        throw CompileError(
            formatDiagnostic(call.arguments[index]->location, DiagnosticStage::TypeCheck,
                             formatBuiltinPerArgumentMismatch(
                                 descriptor.name, descriptor.parameters[index], actual.name())));
      }
    }

    return Type(descriptor.returnKind);
  }

  Type TypeChecker::checkRvalue(const ast::Expression& expression) {
    ExpressionVisitor visitor(*this);
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

  bool TypeChecker::isAssignable(Type expected, Type actual) const {
    return expected == actual;
  }

  void TypeChecker::collectFunctionSignatures(const ast::Module& module) {
    for (const auto& function : module.functions) {

      if (functions_.contains(function.name)) {
        throw CompileError(formatDiagnostic(function.location, DiagnosticStage::TypeCheck,
                                            "duplicate function '" + function.name + "'"));
      }

      FunctionSignature signature;
      requireKnownType(function.returnType, function.location);
      signature.returnType = function.returnType;

      for (const auto& parameter : function.parameters) {
        requireKnownType(parameter.type, parameter.location);
        signature.parameterTypes.push_back(parameter.type);
      }

      functions_.emplace(function.name, std::move(signature));
    }
  }

  void TypeChecker::pushScope() {
    scopes_.emplace_back();
  }

  void TypeChecker::popScope() {
    scopes_.pop_back();
  }

} // namespace noria
