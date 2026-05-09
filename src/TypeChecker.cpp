#include "noria/TypeChecker.hpp"

#include "noria/Diagnostic.hpp"

#include <sstream>

namespace noria {
  namespace {
    // string(location)
    std::string atLocation(SourceLocation location, const std::string& message);
  } // namespace

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

    const Type expectedReturnType = parseTypeName(function.returnType, function.location);

    for (const auto& parameter : function.parameters) {
      const Type parameterType = parseTypeName(parameter.typeName, parameter.location);

      if (!declareLocal(parameter.name, parameterType)) {
        throw CompileError(atLocation(parameter.location,
                                      "typecheck: duplicate parameter '" + parameter.name + "'"));
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
      const Type declaredType = parseTypeName(letStatement->typeName, letStatement->location);
      const Type initializerType = checkExpression(*letStatement->initializer);

      if (!declareLocal(letStatement->name, declaredType)) {
        throw CompileError(
            atLocation(letStatement->location,
                       "typecheck: duplicate local variable '" + letStatement->name + "'"));
      }

      if (!isAssignable(declaredType, initializerType)) {
        throw CompileError(atLocation(letStatement->initializer->location,
                                      "typecheck: cannot initialize '" + letStatement->name +
                                          "' of type " + typeName(declaredType) + " with " +
                                          typeName(initializerType)));
      }

      return false;
    }

    if (const auto* assignmentStatement =
            dynamic_cast<const ast::AssignmentStatement*>(&statement)) {

      const Type targetType = lookupLocal(assignmentStatement->lhs, assignmentStatement->location);
      const Type valueType = checkExpression(*assignmentStatement->rhs);

      if (!isAssignable(targetType, valueType)) {
        throw CompileError(atLocation(assignmentStatement->rhs->location,
                                      "typecheck: cannot assign " + typeName(valueType) +
                                          " to variable '" + assignmentStatement->lhs +
                                          "' of type " + typeName(targetType)));
      }

      return false;
    }

    if (const auto* returnStatement = dynamic_cast<const ast::ReturnStatement*>(&statement)) {

      const Type returnType = checkExpression(*returnStatement->expression);

      if (!isAssignable(expectedReturnType, returnType)) {
        throw CompileError(atLocation(returnStatement->expression->location,
                                      "typecheck: return type " + typeName(returnType) +
                                          " does not match expected " +
                                          typeName(expectedReturnType)));
      }
      return true;
    }

    if (const auto* ifStatement = dynamic_cast<const ast::IfStatement*>(&statement)) {
      const Type conditionType = checkExpression(*ifStatement->condition);
      if (conditionType != Type::Bool) {
        throw CompileError(
            atLocation(ifStatement->condition->location,
                       "typecheck: if condition must be bool, got " + typeName(conditionType)));
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
      if (conditionType != Type::Bool) {
        throw CompileError(
            atLocation(whileStatement->condition->location,
                       "typecheck: while condition must be bool, got " + typeName(conditionType)));
      }

      pushScope();
      checkStatements(whileStatement->body, expectedReturnType);
      popScope();
      return false;
    }

    throw CompileError(atLocation(statement.location, "typecheck: unsupported statement"));
  }

  Type TypeChecker::checkExpression(const ast::Expression& expression) {
    if (dynamic_cast<const ast::IntegerLiteral*>(&expression))
      return Type::I32;

    if (dynamic_cast<const ast::BoolLiteral*>(&expression))
      return Type::Bool;

    if (const auto* identifier = dynamic_cast<const ast::IdentifierExpression*>(&expression))
      return lookupLocal(identifier->name, identifier->location);

    if (const auto* binary = dynamic_cast<const ast::BinaryExpression*>(&expression)) {
      const Type left = checkExpression(*binary->left);
      const Type right = checkExpression(*binary->right);

      if (left != Type::I32 || right != Type::I32) {
        throw CompileError(
            atLocation(binary->location, "typecheck: binary operator requires i32 operands, got " +
                                             typeName(left) + " and " + typeName(right)));
      }

      switch (binary->op) {
      case ast::BinaryOperator::Add:
      case ast::BinaryOperator::Subtract:
      case ast::BinaryOperator::Multiply:
      case ast::BinaryOperator::Divide:
        return Type::I32;
      case ast::BinaryOperator::Less:
      case ast::BinaryOperator::LessEqual:
      case ast::BinaryOperator::Greater:
      case ast::BinaryOperator::GreaterEqual:
      case ast::BinaryOperator::Equal:
      case ast::BinaryOperator::NotEqual:
        return Type::Bool;
      }
    }

    if (const auto* call = dynamic_cast<const ast::CallExpression*>(&expression)) {
      const auto function = functions_.find(call->callee);

      if (function == functions_.end()) {
        throw CompileError(
            atLocation(call->location, "typecheck: unknown function '" + call->callee + "'"));
      }

      if (call->arguments.size() != function->second.parameterTypes.size()) {
        std::ostringstream out;
        out << "typecheck: function '" << call->callee << "' expects "
            << function->second.parameterTypes.size() << " argument(s), got "
            << call->arguments.size();
        throw CompileError(atLocation(call->location, out.str()));
      }

      for (std::size_t index{}; index < call->arguments.size(); ++index) {
        const Type actual = checkExpression(*call->arguments[index]);
        const Type expected = function->second.parameterTypes[index];
        if (!isAssignable(expected, actual)) {
          std::ostringstream out;
          out << "typecheck: argument " << (index + 1) << " of '" << call->callee << "' expects "
              << typeName(expected) << ", got " << typeName(actual);
          throw CompileError(atLocation(call->arguments[index]->location, out.str()));
        }
      }

      return function->second.returnType;
    }

    throw CompileError(atLocation(expression.location, "typecheck: unsupported expression"));
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

    throw CompileError(atLocation(location, "typecheck: unknown local variable '" + name + "'"));
  }

  Type TypeChecker::parseTypeName(const std::string& typeName, SourceLocation location) const {
    if (typeName == "i32")
      return Type::I32;

    if (typeName == "bool")
      return Type::Bool;

    throw CompileError(atLocation(location, "typecheck: unknown type '" + typeName + "'"));
  }

  std::string TypeChecker::typeName(Type type) const {
    switch (type) {
    case Type::I32:
      return "i32";
    case Type::Bool:
      return "bool";
    }

    return "<unknown>";
  }

  bool TypeChecker::isAssignable(Type expected, Type actual) const {
    return expected == actual;
  }

  void TypeChecker::collectFunctionSignatures(const ast::Module& module) {
    for (const auto& function : module.functions) {

      if (functions_.contains(function.name)) {
        throw CompileError(
            atLocation(function.location, "typecheck: duplicate function '" + function.name + "'"));
      }

      FunctionSignature signature;
      signature.returnType = parseTypeName(function.returnType, function.location);

      for (const auto& parameter : function.parameters) {
        signature.parameterTypes.push_back(parseTypeName(parameter.typeName, parameter.location));
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

  namespace {

    std::string atLocation(SourceLocation location, const std::string& message) {
      std::ostringstream out;
      out << location.line << ":" << location.column << ": " << message;
      return out.str();
    }

  } // namespace

} // namespace noria
