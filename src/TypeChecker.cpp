#include "noria/TypeChecker.hpp"

#include "noria/Builtins.hpp"
#include "noria/Diagnostic.hpp"

#include <sstream>

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
    const Type initializerType = checker_.checkExpression(*letStatement.initializer);

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
    const Type targetType =
        checker_.lookupLocal(assignmentStatement.lhs, assignmentStatement.location);
    const Type valueType = checker_.checkExpression(*assignmentStatement.rhs);

    if (!checker_.isAssignable(targetType, valueType)) {
      throw CompileError(
          formatDiagnostic(assignmentStatement.rhs->location, DiagnosticStage::TypeCheck,
                           "cannot assign " + valueType.name() + " to variable '" +
                               assignmentStatement.lhs + "' of type " + targetType.name()));
    }

    returned_ = false;
  }

  void TypeChecker::StatementVisitor::visit(const ast::ReturnStatement& returnStatement) {
    const Type returnType = checker_.checkExpression(*returnStatement.expression);

    if (!checker_.isAssignable(expectedReturnType_, returnType)) {
      throw CompileError(
          formatDiagnostic(returnStatement.expression->location, DiagnosticStage::TypeCheck,
                           "return type " + returnType.name() + " does not match expected " +
                               expectedReturnType_.name()));
    }

    returned_ = true;
  }

  void TypeChecker::StatementVisitor::visit(const ast::IfStatement& ifStatement) {
    const Type conditionType = checker_.checkExpression(*ifStatement.condition);
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
    const Type conditionType = checker_.checkExpression(*whileStatement.condition);
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

    const Type expressionType = checker_.checkExpression(*expressionStatement.expression);
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
    const Type left = checker_.checkExpression(*binary.left);
    const Type right = checker_.checkExpression(*binary.right);

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
    const Type operandType = checker_.checkExpression(*unary.operand);

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
    const Type sourceType = checker_.checkExpression(*castExpression.expression);
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
      const Type actual = checker_.checkExpression(*call.arguments[index]);
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

  void TypeChecker::CallExpressionProbe::visit(const ast::ReturnStatement&) {}
  void TypeChecker::CallExpressionProbe::visit(const ast::LetStatement&) {}
  void TypeChecker::CallExpressionProbe::visit(const ast::IfStatement&) {}
  void TypeChecker::CallExpressionProbe::visit(const ast::WhileStatement&) {}
  void TypeChecker::CallExpressionProbe::visit(const ast::AssignmentStatement&) {}
  void TypeChecker::CallExpressionProbe::visit(const ast::ExpressionStatement&) {}

  void TypeChecker::requireKnownType(const Type& type, SourceLocation location) const {
    if (type == Type::i32() || type == Type::f64() || type == Type::boolean() ||
        type == Type::str())
      return;

    throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                        "unknown type '" + type.name() + "'"));
  }

  void TypeChecker::check(const ast::Module& module) {
    functions_.clear();
    scopes_.clear();

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

    if (descriptor.style == MismatchStyle::AllArguments) {
      const Type firstType = checkExpression(*call.arguments[0]);
      const Type secondType = checkExpression(*call.arguments[1]);
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
      const Type actual = checkExpression(*call.arguments[index]);
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

  Type TypeChecker::checkExpression(const ast::Expression& expression) {
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
