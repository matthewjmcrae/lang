#include "noria/TypeChecker.hpp"

#include "noria/Diagnostic.hpp"

#include <sstream>

namespace noria {
  namespace {
    std::string atLocation(SourceLocation location, const std::string& message);

    bool isBuiltinName(const std::string& name) {
      return name == "print" || name == "print_int" || name == "print_float" ||
             name == "print_char" || name == "println" || name == "sqrt" || name == "pow";
    }
  } // namespace

  Type Type::array(Type elementType) {
    Type type(TypeKind::Array);
    type.element = std::make_shared<Type>(std::move(elementType));
    return type;
  }

  Type Type::structType(std::string name) {
    Type type(TypeKind::Struct);
    type.structName = std::move(name);
    return type;
  }

  bool Type::operator==(const Type& other) const {
    if (kind != other.kind)
      return false;

    switch (kind) {
    case TypeKind::Array:
      if (!element || !other.element)
        return element == other.element;
      return *element == *other.element;
    case TypeKind::Struct:
      return structName == other.structName;
    default:
      return true;
    }
  }

  std::string Type::name() const {
    switch (kind) {
    case TypeKind::I32:
      return "i32";
    case TypeKind::F64:
      return "f64";
    case TypeKind::Bool:
      return "bool";
    case TypeKind::Str:
      return "str";
    case TypeKind::Array:
      return "[" + (element ? element->name() : std::string{"?"}) + "]";
    case TypeKind::Struct:
      return structName.empty() ? std::string{"<struct>"} : structName;
    case TypeKind::Void:
      return "void";
    }

    return "<unknown>";
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
      if (conditionType != Type::boolean()) {
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
      if (conditionType != Type::boolean()) {
        throw CompileError(
            atLocation(whileStatement->condition->location,
                       "typecheck: while condition must be bool, got " + typeName(conditionType)));
      }

      pushScope();
      checkStatements(whileStatement->body, expectedReturnType);
      popScope();
      return false;
    }

    if (const auto* expressionStatement =
            dynamic_cast<const ast::ExpressionStatement*>(&statement)) {
      if (!dynamic_cast<const ast::CallExpression*>(expressionStatement->expression.get())) {
        throw CompileError(atLocation(expressionStatement->location,
                                      "typecheck: expression statement must be a function call"));
      }

      const Type expressionType = checkExpression(*expressionStatement->expression);
      if (expressionType != Type::voidType()) {
        throw CompileError(atLocation(expressionStatement->expression->location,
                                      "typecheck: expression statement must call a void builtin"));
      }

      return false;
    }

    throw CompileError(atLocation(statement.location, "typecheck: unsupported statement"));
  }

  Type TypeChecker::checkBuiltinCall(const ast::CallExpression& call) {
    const std::string& name = call.callee;

    if (name == "print") {
      if (call.arguments.size() != 1)
        throw CompileError(atLocation(call.location, "typecheck: print expects 1 argument"));
      const Type argType = checkExpression(*call.arguments[0]);
      if (argType != Type::str())
        throw CompileError(atLocation(call.arguments[0]->location,
                                      "typecheck: print expects str, got " + typeName(argType)));
      return Type::voidType();
    }

    if (name == "print_int") {
      if (call.arguments.size() != 1)
        throw CompileError(atLocation(call.location, "typecheck: print_int expects 1 argument"));
      const Type argType = checkExpression(*call.arguments[0]);
      if (argType != Type::i32())
        throw CompileError(atLocation(call.arguments[0]->location,
                                      "typecheck: print_int expects i32, got " + typeName(argType)));
      return Type::voidType();
    }

    if (name == "print_float") {
      if (call.arguments.size() != 1)
        throw CompileError(atLocation(call.location, "typecheck: print_float expects 1 argument"));
      const Type argType = checkExpression(*call.arguments[0]);
      if (argType != Type::f64())
        throw CompileError(atLocation(call.arguments[0]->location,
                                      "typecheck: print_float expects f64, got " + typeName(argType)));
      return Type::voidType();
    }

    if (name == "print_char") {
      if (call.arguments.size() != 1)
        throw CompileError(atLocation(call.location, "typecheck: print_char expects 1 argument"));
      const Type argType = checkExpression(*call.arguments[0]);
      if (argType != Type::i32())
        throw CompileError(atLocation(call.arguments[0]->location,
                                      "typecheck: print_char expects i32, got " + typeName(argType)));
      return Type::voidType();
    }

    if (name == "println") {
      if (!call.arguments.empty())
        throw CompileError(atLocation(call.location, "typecheck: println expects 0 arguments"));
      return Type::voidType();
    }

    if (name == "sqrt") {
      if (call.arguments.size() != 1)
        throw CompileError(atLocation(call.location, "typecheck: sqrt expects 1 argument"));
      const Type argType = checkExpression(*call.arguments[0]);
      if (argType != Type::f64())
        throw CompileError(atLocation(call.arguments[0]->location,
                                      "typecheck: sqrt expects f64, got " + typeName(argType)));
      return Type::f64();
    }

    if (name == "pow") {
      if (call.arguments.size() != 2)
        throw CompileError(atLocation(call.location, "typecheck: pow expects 2 arguments"));
      const Type baseType = checkExpression(*call.arguments[0]);
      const Type expType = checkExpression(*call.arguments[1]);
      if (baseType != Type::f64() || expType != Type::f64())
        throw CompileError(atLocation(call.location, "typecheck: pow expects f64 arguments, got " +
                                                          typeName(baseType) + " and " +
                                                          typeName(expType)));
      return Type::f64();
    }

    throw CompileError("internal typecheck error: unknown builtin");
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
          throw CompileError(atLocation(
              binary->location, "typecheck: logical operator requires bool operands, got " +
                                    typeName(left) + " and " + typeName(right)));
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
        throw CompileError(atLocation(
            binary->location, "typecheck: arithmetic operator requires matching numeric operands, got " +
                                  typeName(left) + " and " + typeName(right)));
      case ast::BinaryOperator::Modulo:
      case ast::BinaryOperator::BitAnd:
      case ast::BinaryOperator::BitOr:
      case ast::BinaryOperator::BitXor:
      case ast::BinaryOperator::Shl:
      case ast::BinaryOperator::Shr:
        if (left != Type::i32() || right != Type::i32()) {
          throw CompileError(atLocation(
              binary->location, "typecheck: integer operator requires i32 operands, got " +
                                    typeName(left) + " and " + typeName(right)));
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
        throw CompileError(
            atLocation(binary->location, "typecheck: comparison requires matching numeric operands, got " +
                                             typeName(left) + " and " + typeName(right)));
      }
    }

    if (const auto* unary = dynamic_cast<const ast::UnaryExpression*>(&expression)) {
      const Type operandType = checkExpression(*unary->operand);

      switch (unary->op) {
      case ast::UnaryOperator::Negate:
        if (operandType == Type::i32() || operandType == Type::f64())
          return operandType;
        throw CompileError(atLocation(unary->location,
                                     "typecheck: unary negation requires numeric operand, got " +
                                         typeName(operandType)));
      case ast::UnaryOperator::BitNot:
        if (operandType != Type::i32()) {
          throw CompileError(atLocation(unary->location,
                                       "typecheck: unary operator requires i32 operand, got " +
                                           typeName(operandType)));
        }
        return Type::i32();
      case ast::UnaryOperator::Not:
        if (operandType != Type::boolean()) {
          throw CompileError(atLocation(unary->location,
                                       "typecheck: logical not requires bool operand, got " +
                                           typeName(operandType)));
        }
        return Type::boolean();
      }
    }

    if (const auto* castExpression = dynamic_cast<const ast::CastExpression*>(&expression)) {
      const Type sourceType = checkExpression(*castExpression->expression);
      const Type targetType =
          parseTypeName(castExpression->targetTypeName, castExpression->location);

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

      throw CompileError(atLocation(castExpression->location, "typecheck: cannot cast " +
                                                                    typeName(sourceType) + " to " +
                                                                    typeName(targetType)));
    }

    if (const auto* call = dynamic_cast<const ast::CallExpression*>(&expression)) {
      if (isBuiltinName(call->callee))
        return checkBuiltinCall(*call);

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
      return Type::i32();

    if (typeName == "f64")
      return Type::f64();

    if (typeName == "bool")
      return Type::boolean();

    if (typeName == "str")
      return Type::str();

    throw CompileError(atLocation(location, "typecheck: unknown type '" + typeName + "'"));
  }

  std::string TypeChecker::typeName(Type type) const {
    return type.name();
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
