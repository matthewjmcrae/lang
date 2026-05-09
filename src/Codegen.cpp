#include "noria/Codegen.hpp"

#include "noria/Diagnostic.hpp"

#include <sstream>
#include <unordered_map>

namespace noria {

  namespace {
    bool isComparison(ast::BinaryOperator op);
    std::string llvmArithmeticInstruction(ast::BinaryOperator op);
    std::string llvmComparisonPredicate(ast::BinaryOperator op);
  } // namespace

  // main flow
  // gen() -> genFunction() + push scope (pop when done)-> genStatement() -> push scope if needed ->
  // genStatement() ->.... ->pop scope
  //                                                                      ->  genExpression() ->....
  std::string LlvmIrTextGenerator::generate(const ast::Module& module) const {
    collectFunctionBindings(module);

    std::ostringstream out;

    for (const auto& function : module.functions) {
      out << generateFunction(function) << "\n";
    }

    return out.str();
  }

  std::string LlvmIrTextGenerator::generateFunction(const ast::Function& function) const {
    const IrType returnType = parseIrType(function.returnType);

    std::ostringstream out;
    out << "define " << llvmType(returnType) << " @" << function.name << "(";
    for (std::size_t index{}; index < function.parameters.size(); ++index) {
      const auto& parameter = function.parameters[index];
      const IrType parameterType = parseIrType(parameter.typeName);

      if (index != 0)
        out << ", ";

      out << llvmType(parameterType) << " %" << parameter.name << ".param";
    }
    out << ") {\n";
    out << "entry:\n";

    int nextTemporary = 0; // unique LLVM register names
    int nextLabel = 0;     // unique LLVM label names
    std::vector<Scope> scopes;
    scopes.emplace_back(); // scope is an unordered_map, create an empty scope

    for (const auto& parameter : function.parameters) {
      const IrType parameterType = parseIrType(parameter.typeName);
      if (!declareLocal(scopes, parameter.name,
                        LocalBinding{"%" + parameter.name, parameterType})) {
        throw CompileError("codegen: duplicate parameter '" + parameter.name + "'");
      }

      // allocate parameter on the stack
      const std::string slot = "%" + parameter.name;
      out << "  " << slot << " = alloca " << llvmType(parameterType) << "\n";
      out << "  store " << llvmType(parameterType) << " %" << parameter.name << ".param, ptr "
          << slot << "\n";
    }

    const bool emittedReturn =
        generateStatements(function.body, out, nextTemporary, nextLabel, returnType, scopes);

    if (!emittedReturn) {
      out << "  ret " << llvmType(returnType) << " " << (returnType == IrType::Bool ? "false" : "0")
          << "\n";
    }

    out << "}\n";
    return out.str();
  }

  bool LlvmIrTextGenerator::generateStatements(
      const std::vector<std::unique_ptr<ast::Statement>>& statements, std::ostringstream& out,
      int& nextTemporary, int& nextLabel, IrType expectedReturnType,
      std::vector<Scope>& scopes) const {

    for (const auto& statement : statements) {
      if (generateStatement(*statement, out, nextTemporary, nextLabel, expectedReturnType, scopes))
        return true;
    }

    return false;
  }

  bool LlvmIrTextGenerator::generateStatement(const ast::Statement& statement,
                                              std::ostringstream& out, int& nextTemporary,
                                              int& nextLabel, IrType expectedReturnType,
                                              std::vector<Scope>& scopes) const {

    if (const auto* letStatement = dynamic_cast<const ast::LetStatement*>(&statement)) {
      const IrType localType = parseIrType(letStatement->typeName);
      const std::string slot = "%" + letStatement->name + ".slot" + std::to_string(nextTemporary++);

      if (!declareLocal(scopes, letStatement->name, LocalBinding{slot, localType})) {
        throw CompileError("codegen: duplicate local variable '" + letStatement->name + "'");
      }

      out << "  " << slot << " = alloca " << llvmType(localType) << "\n";

      // store new variable on the stack
      Value initializer =
          generateExpression(*letStatement->initializer, out, nextTemporary, scopes);
      out << "  store " << llvmType(localType) << " " << initializer.text << ", ptr " << slot
          << "\n";
      return false;
    }

    if (const auto* returnStatement = dynamic_cast<const ast::ReturnStatement*>(&statement)) {
      Value returnValue =
          generateExpression(*returnStatement->expression, out, nextTemporary, scopes);
      out << "  ret " << llvmType(expectedReturnType) << " " << returnValue.text << "\n";
      return true;
    }

    if (const auto* assignmentStatement =
            dynamic_cast<const ast::AssignmentStatement*>(&statement)) {
      const LocalBinding& local = lookupLocal(scopes, assignmentStatement->lhs);

      // update local value on the stack
      Value rvalue = generateExpression(*assignmentStatement->rhs, out, nextTemporary, scopes);
      out << "  store " << llvmType(local.type) << " " << rvalue.text << ", ptr " << local.slot
          << "\n";
      return false;
    }

    if (const auto* ifStatement = dynamic_cast<const ast::IfStatement*>(&statement)) {
      const int labelId = nextLabel++;
      const std::string thenLabel = "if.then" + std::to_string(labelId);
      const std::string elseLabel = "if.else" + std::to_string(labelId);
      const std::string endLabel = "if.end" + std::to_string(labelId);

      const std::string condition =
          generateCondition(*ifStatement->condition, out, nextTemporary, scopes);
      out << "  br i1 " << condition << ", label %" << thenLabel << ", label %" << elseLabel
          << "\n";

      out << thenLabel << ":\n";
      scopes.emplace_back();
      const bool thenReturns = generateStatements(ifStatement->thenBranch, out, nextTemporary,
                                                  nextLabel, expectedReturnType, scopes);
      scopes.pop_back();

      // if branches dont return, continue into the merged code block

      /*  if cond{
       *    x=1
       *  }
       *  else{
       *    x=2
       *  }
       *  return x; <- merged block
       */

      if (!thenReturns)
        out << "  br label %" << endLabel << "\n";

      out << elseLabel << ":\n";
      scopes.emplace_back();
      const bool elseReturns = generateStatements(ifStatement->elseBranch, out, nextTemporary,
                                                  nextLabel, expectedReturnType, scopes);
      scopes.pop_back();
      if (!elseReturns)
        out << "  br label %" << endLabel << "\n";

      // if either branch does not return then we have a merged block after
      if (!thenReturns || !elseReturns) {
        out << endLabel << ":\n";
        return false;
      }

      return true;
    }

    if (const auto* whileStatement = dynamic_cast<const ast::WhileStatement*>(&statement)) {
      const int labelId = nextLabel++;
      const std::string conditionLabel = "while.cond" + std::to_string(labelId);
      const std::string bodyLabel = "while.body" + std::to_string(labelId);
      const std::string endLabel = "while.end" + std::to_string(labelId);

      out << "  br label %" << conditionLabel << "\n";

      out << conditionLabel << ":\n";
      const std::string condition =
          generateCondition(*whileStatement->condition, out, nextTemporary, scopes);
      out << "  br i1 " << condition << ", label %" << bodyLabel << ", label %" << endLabel << "\n";

      out << bodyLabel << ":\n";
      scopes.emplace_back();
      const bool bodyReturns = generateStatements(whileStatement->body, out, nextTemporary,
                                                  nextLabel, expectedReturnType, scopes);
      scopes.pop_back();
      if (!bodyReturns)
        out << "  br label %" << conditionLabel << "\n";

      out << endLabel << ":\n";
      return false;
    }

    throw CompileError("codegen: unsupported statement");
  }

  std::string LlvmIrTextGenerator::generateCondition(const ast::Expression& expression,
                                                     std::ostringstream& out, int& nextTemporary,
                                                     const std::vector<Scope>& scopes) const {

    if (const auto* binary = dynamic_cast<const ast::BinaryExpression*>(&expression))
      if (isComparison(binary->op))
        return generateBinaryExpression(*binary, out, nextTemporary, scopes).text;

    const Value value = generateExpression(expression, out, nextTemporary, scopes);

    if (value.type == IrType::Bool)
      return value.text;

    const std::string result = "%t" + std::to_string(nextTemporary++);
    out << "  " << result << " = icmp ne i32 " << value.text << ", 0\n";
    return result;
  }

  LlvmIrTextGenerator::Value
  LlvmIrTextGenerator::generateExpression(const ast::Expression& expression,
                                          std::ostringstream& out, int& nextTemporary,
                                          const std::vector<Scope>& scopes) const {

    if (const auto* integer = dynamic_cast<const ast::IntegerLiteral*>(&expression))
      return Value{std::to_string(integer->value), IrType::I32};

    if (const auto* boolean = dynamic_cast<const ast::BoolLiteral*>(&expression))
      return Value{boolean->value ? "true" : "false", IrType::Bool};

    if (const auto* binary = dynamic_cast<const ast::BinaryExpression*>(&expression))
      return generateBinaryExpression(*binary, out, nextTemporary, scopes);

    if (const auto* identifier = dynamic_cast<const ast::IdentifierExpression*>(&expression)) {
      const LocalBinding& local = lookupLocal(scopes, identifier->name);

      const std::string result = "%t" + std::to_string(nextTemporary++);
      out << "  " << result << " = load " << llvmType(local.type) << ", ptr " << local.slot << "\n";
      return Value{result, local.type};
    }

    if (const auto* call = dynamic_cast<const ast::CallExpression*>(&expression)) {
      const auto function = functions_.find(call->callee);
      if (function == functions_.end())
        throw CompileError("codegen: unknown function '" + call->callee + "'");

      std::vector<Value> arguments;
      arguments.reserve(call->arguments.size());

      for (const auto& argument : call->arguments) {
        arguments.push_back(generateExpression(*argument, out, nextTemporary, scopes));
      }

      const std::string result = "%t" + std::to_string(nextTemporary++);
      out << "  " << result << " = call " << llvmType(function->second.returnType) << " @"
          << call->callee << "(";
      for (std::size_t index{}; index < arguments.size(); ++index) {
        if (index != 0)
          out << ", ";

        out << llvmType(arguments[index].type) << " " << arguments[index].text;
      }
      out << ")\n";
      return Value{result, function->second.returnType};
    }

    throw CompileError("codegen: unsupported expression");
  }

  LlvmIrTextGenerator::Value
  LlvmIrTextGenerator::generateBinaryExpression(const ast::BinaryExpression& binary,
                                                std::ostringstream& out, int& nextTemporary,
                                                const std::vector<Scope>& scopes) const {

    const Value left = generateExpression(*binary.left, out, nextTemporary, scopes);
    const Value right = generateExpression(*binary.right, out, nextTemporary, scopes);
    const std::string result = "%t" + std::to_string(nextTemporary++);

    if (isComparison(binary.op)) {
      out << "  " << result << " = icmp " << llvmComparisonPredicate(binary.op) << " i32 "
          << left.text << ", " << right.text << "\n";
      return Value{result, IrType::Bool};
    }

    out << "  " << result << " = " << llvmArithmeticInstruction(binary.op) << " i32 " << left.text
        << ", " << right.text << "\n";
    return Value{result, IrType::I32};
  }

  LlvmIrTextGenerator::IrType LlvmIrTextGenerator::parseIrType(const std::string& typeName) const {
    if (typeName == "i32")
      return IrType::I32;

    if (typeName == "bool")
      return IrType::Bool;

    throw CompileError("codegen: unknown type '" + typeName + "'");
  }

  std::string LlvmIrTextGenerator::llvmType(IrType type) const {
    switch (type) {
    case IrType::I32:
      return "i32";
    case IrType::Bool:
      return "i1";
    }

    return "";
  }

  bool LlvmIrTextGenerator::declareLocal(std::vector<Scope>& scopes, const std::string& name,
                                         LocalBinding binding) const {
    if (scopes.empty())
      scopes.emplace_back();

    auto& scope = scopes.back();
    if (scope.contains(name))
      return false;

    scope.emplace(name, std::move(binding));
    return true;
  }

  const LlvmIrTextGenerator::LocalBinding&
  LlvmIrTextGenerator::lookupLocal(const std::vector<Scope>& scopes,
                                   const std::string& name) const {

    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
      const auto local = scope->find(name);
      if (local != scope->end())
        return local->second;
    }

    throw CompileError("codegen: unknown local variable '" + name + "'");
  }

  void LlvmIrTextGenerator::collectFunctionBindings(const ast::Module& module) const {
    functions_.clear();

    for (const auto& function : module.functions) {
      FunctionBinding binding;
      binding.returnType = parseIrType(function.returnType);
      for (const auto& parameter : function.parameters) {
        binding.parameterTypes.push_back(parseIrType(parameter.typeName));
      }
      functions_.emplace(function.name, std::move(binding));
    }
  }

  namespace {

    bool isComparison(ast::BinaryOperator op) {
      switch (op) {
      case ast::BinaryOperator::Less:
      case ast::BinaryOperator::LessEqual:
      case ast::BinaryOperator::Greater:
      case ast::BinaryOperator::GreaterEqual:
      case ast::BinaryOperator::Equal:
      case ast::BinaryOperator::NotEqual:
        return true;
      case ast::BinaryOperator::Add:
      case ast::BinaryOperator::Subtract:
      case ast::BinaryOperator::Multiply:
      case ast::BinaryOperator::Divide:
        return false;
      }

      return false;
    }

    std::string llvmArithmeticInstruction(ast::BinaryOperator op) {
      switch (op) {
      case ast::BinaryOperator::Add:
        return "add";
      case ast::BinaryOperator::Subtract:
        return "sub";
      case ast::BinaryOperator::Multiply:
        return "mul";
      case ast::BinaryOperator::Divide:
        return "sdiv";
      case ast::BinaryOperator::Less:
      case ast::BinaryOperator::LessEqual:
      case ast::BinaryOperator::Greater:
      case ast::BinaryOperator::GreaterEqual:
      case ast::BinaryOperator::Equal:
      case ast::BinaryOperator::NotEqual:
        break;
      }

      return "";
    }

    std::string llvmComparisonPredicate(ast::BinaryOperator op) {
      switch (op) {
      case ast::BinaryOperator::Less:
        return "slt";
      case ast::BinaryOperator::LessEqual:
        return "sle";
      case ast::BinaryOperator::Greater:
        return "sgt";
      case ast::BinaryOperator::GreaterEqual:
        return "sge";
      case ast::BinaryOperator::Equal:
        return "eq";
      case ast::BinaryOperator::NotEqual:
        return "ne";
      case ast::BinaryOperator::Add:
      case ast::BinaryOperator::Subtract:
      case ast::BinaryOperator::Multiply:
      case ast::BinaryOperator::Divide:
        break;
      }

      return "";
    }

  } // namespace

} // namespace noria
