#include "noria/TypeChecker.hpp"

#include "noria/Builtins.hpp"
#include "noria/Diagnostic.hpp"

#include <sstream>

namespace noria {

  void TypeChecker::requireKnownType(const Type& type, SourceLocation location) const {
    if (type == Type::i32() || type == Type::f64() || type == Type::boolean() ||
        type == Type::str())
      return;

    throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                        "unknown type '" + type.name() + "'"));
  }

  // main flow
  // check() -> checkFunction() + push scope (pop when done)-> checkStatement() -> push scope if
  // needed -> checkStatement() ->.... ->pop scope
  //                                                                            -> checkExpression()
  //                                                                            -> ....
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

    if (const auto* letStatement = dynamic_cast<const ast::LetStatement*>(&statement)) {
      requireKnownType(letStatement->type, letStatement->location);
      const Type declaredType = letStatement->type;
      const Type initializerType = checkExpression(*letStatement->initializer);

      if (!declareLocal(letStatement->name, declaredType)) {
        throw CompileError(
            formatDiagnostic(letStatement->location, DiagnosticStage::TypeCheck,
                             "duplicate local variable '" + letStatement->name + "'"));
      }

      if (!isAssignable(declaredType, initializerType)) {
        throw CompileError(
            formatDiagnostic(letStatement->initializer->location, DiagnosticStage::TypeCheck,
                             "cannot initialize '" + letStatement->name + "' of type " +
                                 declaredType.name() + " with " + initializerType.name()));
      }

      return false;
    }

    if (const auto* assignmentStatement =
            dynamic_cast<const ast::AssignmentStatement*>(&statement)) {

      const Type targetType = lookupLocal(assignmentStatement->lhs, assignmentStatement->location);
      const Type valueType = checkExpression(*assignmentStatement->rhs);

      if (!isAssignable(targetType, valueType)) {
        throw CompileError(
            formatDiagnostic(assignmentStatement->rhs->location, DiagnosticStage::TypeCheck,
                             "cannot assign " + valueType.name() + " to variable '" +
                                 assignmentStatement->lhs + "' of type " + targetType.name()));
      }

      return false;
    }

    if (const auto* returnStatement = dynamic_cast<const ast::ReturnStatement*>(&statement)) {

      const Type returnType = checkExpression(*returnStatement->expression);

      if (!isAssignable(expectedReturnType, returnType)) {
        throw CompileError(
            formatDiagnostic(returnStatement->expression->location, DiagnosticStage::TypeCheck,
                             "return type " + returnType.name() + " does not match expected " +
                                 expectedReturnType.name()));
      }
      return true;
    }

    if (const auto* ifStatement = dynamic_cast<const ast::IfStatement*>(&statement)) {
      const Type conditionType = checkExpression(*ifStatement->condition);
      if (conditionType != Type::boolean()) {
        throw CompileError(
            formatDiagnostic(ifStatement->condition->location, DiagnosticStage::TypeCheck,
                             "if condition must be bool, got " + conditionType.name()));
      }

      pushScope();
      const bool thenReturns = checkStatements(ifStatement->thenBranch, expectedReturnType);
      popScope();

      pushScope();
      const bool elseReturns = checkStatements(ifStatement->elseBranch, expectedReturnType);
      popScope();

      return thenReturns && elseReturns;
    }

    if (const auto* whileStatement = dynamic_cast<const ast::WhileStatement*>(&statement)) {
      const Type conditionType = checkExpression(*whileStatement->condition);
      if (conditionType != Type::boolean()) {
        throw CompileError(
            formatDiagnostic(whileStatement->condition->location, DiagnosticStage::TypeCheck,
                             "while condition must be bool, got " + conditionType.name()));
      }

      pushScope();
      checkStatements(whileStatement->body, expectedReturnType);
      popScope();
      return false;
    }

    if (const auto* expressionStatement =
            dynamic_cast<const ast::ExpressionStatement*>(&statement)) {
      if (!dynamic_cast<const ast::CallExpression*>(expressionStatement->expression.get())) {
        throw CompileError(formatDiagnostic(expressionStatement->location,
                                            DiagnosticStage::TypeCheck,
                                            "expression statement must be a function call"));
      }

      const Type expressionType = checkExpression(*expressionStatement->expression);
      if (expressionType != Type::voidType()) {
        throw CompileError(formatDiagnostic(expressionStatement->expression->location,
                                            DiagnosticStage::TypeCheck,
                                            "expression statement must call a void builtin"));
      }

      return false;
    }

    throw CompileError(
        formatDiagnostic(statement.location, DiagnosticStage::TypeCheck, "unsupported statement"));
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
    if (dynamic_cast<const ast::IntegerLiteral*>(&expression))
      return Type::i32();

    if (dynamic_cast<const ast::FloatLiteral*>(&expression))
      return Type::f64();

    if (dynamic_cast<const ast::StringLiteral*>(&expression))
      return Type::str();

    if (dynamic_cast<const ast::BoolLiteral*>(&expression))
      return Type::boolean();

    if (const auto* identifier = dynamic_cast<const ast::IdentifierExpression*>(&expression))
      return lookupLocal(identifier->name, identifier->location);

    if (const auto* binary = dynamic_cast<const ast::BinaryExpression*>(&expression)) {
      const Type left = checkExpression(*binary->left);
      const Type right = checkExpression(*binary->right);

      switch (binary->op) {
      case ast::BinaryOperator::And:
      case ast::BinaryOperator::Or:
        if (left != Type::boolean() || right != Type::boolean()) {
          throw CompileError(formatDiagnostic(binary->location, DiagnosticStage::TypeCheck,
                                              "logical operator requires bool operands, got " +
                                                  left.name() + " and " + right.name()));
        }
        return Type::boolean();
      case ast::BinaryOperator::Add:
      case ast::BinaryOperator::Subtract:
      case ast::BinaryOperator::Multiply:
      case ast::BinaryOperator::Divide:
        if (left == Type::f64() && right == Type::f64())
          return Type::f64();
        if (left == Type::i32() && right == Type::i32())
          return Type::i32();
        throw CompileError(
            formatDiagnostic(binary->location, DiagnosticStage::TypeCheck,
                             "arithmetic operator requires matching numeric operands, got " +
                                 left.name() + " and " + right.name()));
      case ast::BinaryOperator::Modulo:
      case ast::BinaryOperator::BitAnd:
      case ast::BinaryOperator::BitOr:
      case ast::BinaryOperator::BitXor:
      case ast::BinaryOperator::Shl:
      case ast::BinaryOperator::Shr:
        if (left != Type::i32() || right != Type::i32()) {
          throw CompileError(formatDiagnostic(binary->location, DiagnosticStage::TypeCheck,
                                              "integer operator requires i32 operands, got " +
                                                  left.name() + " and " + right.name()));
        }
        return Type::i32();
      case ast::BinaryOperator::Less:
      case ast::BinaryOperator::LessEqual:
      case ast::BinaryOperator::Greater:
      case ast::BinaryOperator::GreaterEqual:
      case ast::BinaryOperator::Equal:
      case ast::BinaryOperator::NotEqual:
        if (left == right && (left == Type::i32() || left == Type::f64()))
          return Type::boolean();
        throw CompileError(formatDiagnostic(binary->location, DiagnosticStage::TypeCheck,
                                            "comparison requires matching numeric operands, got " +
                                                left.name() + " and " + right.name()));
      }
    }

    if (const auto* unary = dynamic_cast<const ast::UnaryExpression*>(&expression)) {
      const Type operandType = checkExpression(*unary->operand);

      switch (unary->op) {
      case ast::UnaryOperator::Negate:
        if (operandType == Type::i32() || operandType == Type::f64())
          return operandType;
        throw CompileError(
            formatDiagnostic(unary->location, DiagnosticStage::TypeCheck,
                             "unary negation requires numeric operand, got " + operandType.name()));
      case ast::UnaryOperator::BitNot:
        if (operandType != Type::i32()) {
          throw CompileError(
              formatDiagnostic(unary->location, DiagnosticStage::TypeCheck,
                               "unary operator requires i32 operand, got " + operandType.name()));
        }
        return Type::i32();
      case ast::UnaryOperator::Not:
        if (operandType != Type::boolean()) {
          throw CompileError(
              formatDiagnostic(unary->location, DiagnosticStage::TypeCheck,
                               "logical not requires bool operand, got " + operandType.name()));
        }
        return Type::boolean();
      }
    }

    if (const auto* castExpression = dynamic_cast<const ast::CastExpression*>(&expression)) {
      const Type sourceType = checkExpression(*castExpression->expression);
      requireKnownType(castExpression->targetType, castExpression->location);
      const Type targetType = castExpression->targetType;

      if (sourceType == targetType)
        return targetType;

      if (sourceType == Type::i32() && targetType == Type::f64())
        return Type::f64();
      if (sourceType == Type::f64() && targetType == Type::i32())
        return Type::i32();
      if (sourceType == Type::boolean() && targetType == Type::i32())
        return Type::i32();
      if (sourceType == Type::i32() && targetType == Type::boolean())
        return Type::boolean();

      throw CompileError(
          formatDiagnostic(castExpression->location, DiagnosticStage::TypeCheck,
                           "cannot cast " + sourceType.name() + " to " + targetType.name()));
    }

    if (const auto* call = dynamic_cast<const ast::CallExpression*>(&expression)) {
      if (const BuiltinSignature* descriptor = lookupBuiltin(call->callee))
        return checkBuiltinCall(*call, *descriptor);

      const auto function = functions_.find(call->callee);

      if (function == functions_.end()) {
        throw CompileError(formatDiagnostic(call->location, DiagnosticStage::TypeCheck,
                                            "unknown function '" + call->callee + "'"));
      }

      if (call->arguments.size() != function->second.parameterTypes.size()) {
        std::ostringstream out;
        out << "function '" << call->callee << "' expects "
            << function->second.parameterTypes.size() << " argument(s), got "
            << call->arguments.size();
        throw CompileError(formatDiagnostic(call->location, DiagnosticStage::TypeCheck, out.str()));
      }

      for (std::size_t index{}; index < call->arguments.size(); ++index) {
        const Type actual = checkExpression(*call->arguments[index]);
        const Type expected = function->second.parameterTypes[index];
        if (!isAssignable(expected, actual)) {
          std::ostringstream out;
          out << "argument " << (index + 1) << " of '" << call->callee << "' expects "
              << expected.name() << ", got " << actual.name();
          throw CompileError(formatDiagnostic(call->arguments[index]->location,
                                              DiagnosticStage::TypeCheck, out.str()));
        }
      }

      return function->second.returnType;
    }

    throw CompileError(formatDiagnostic(expression.location, DiagnosticStage::TypeCheck,
                                        "unsupported expression"));
  }

  // helpers
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
    // a scope is an unordered_map, call the default constructor
    scopes_.emplace_back();
  }

  void TypeChecker::popScope() {
    scopes_.pop_back();
  }

} // namespace noria
