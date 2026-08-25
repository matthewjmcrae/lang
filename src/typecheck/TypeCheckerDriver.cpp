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

  namespace {

    bool statementAlwaysReturns(const ast::Statement& statement);
    void requireReturnForms(const std::vector<std::unique_ptr<ast::Statement>>& statements,
                            const Type& returnType);

    bool statementsAlwaysReturn(const std::vector<std::unique_ptr<ast::Statement>>& statements) {
      for (const auto& statement : statements) {
        if (statementAlwaysReturns(*statement)) {
          return true;
        }
      }
      return false;
    }

    bool statementAlwaysReturns(const ast::Statement& statement) {
      if (dynamic_cast<const ast::ReturnStatement*>(&statement) != nullptr) {
        return true;
      }

      const auto* ifStatement = dynamic_cast<const ast::IfStatement*>(&statement);
      return ifStatement != nullptr && !ifStatement->elseBranch.empty() &&
             statementsAlwaysReturn(ifStatement->thenBranch) &&
             statementsAlwaysReturn(ifStatement->elseBranch);
    }

    void requireReturnForm(const ast::ReturnStatement& returnStatement, const Type& returnType) {
      if (returnStatement.expression && returnType == Type::voidType()) {
        throw CompileError(formatDiagnostic(returnStatement.location, DiagnosticStage::TypeCheck,
                                            "void function cannot return a value"));
      }
      if (!returnStatement.expression && returnType != Type::voidType()) {
        throw CompileError(formatDiagnostic(returnStatement.location, DiagnosticStage::TypeCheck,
                                            "non-void function must return a value"));
      }
    }

    void requireReturnForms(const std::vector<std::unique_ptr<ast::Statement>>& statements,
                            const Type& returnType) {
      for (const auto& statement : statements) {
        if (const auto* returnStatement = dynamic_cast<const ast::ReturnStatement*>(statement.get())) {
          requireReturnForm(*returnStatement, returnType);
          continue;
        }
        if (const auto* ifStatement = dynamic_cast<const ast::IfStatement*>(statement.get())) {
          requireReturnForms(ifStatement->thenBranch, returnType);
          requireReturnForms(ifStatement->elseBranch, returnType);
          continue;
        }
        if (const auto* whileStatement = dynamic_cast<const ast::WhileStatement*>(statement.get())) {
          requireReturnForms(whileStatement->body, returnType);
        }
      }
    }

    void requireExplicitReturn(const ast::Function& function) {
      requireReturnForms(function.body, function.returnType);
      if (statementsAlwaysReturn(function.body)) {
        return;
      }
      throw CompileError(formatDiagnostic(
          function.location, DiagnosticStage::TypeCheck,
          "not all control-flow paths in function '" + function.name + "' return"));
    }

  } // namespace

  void TypeChecker::checkImpl(const ast::Module& module, const SymbolOrigins& symbolOrigins) {
    environment_.activeModule = &module;
    environment_.functions.clear();
    environment_.genericFunctions.clear();
    session_.specializationRequests.clear();
    session_.structSpecializationRequests.clear();
    session_.scopes.clear();
    session_.currentFunctionName.clear();
    environment_.symbolOrigins = symbolOrigins;

    for (const auto& function : module.functions) {
      requireExplicitReturn(function);
    }

    collectStructDecls(module);
    collectFunctionSignatures(module);

    for (const auto& function : module.functions) {
      if (function.typeParams.empty()) {
        checkFunction(function);
      }
    }
  }

  void TypeChecker::checkSpecializationFrontierImpl(const ast::Module& module,
                                                      std::size_t firstNewStruct,
                                                      std::size_t firstNewFunction,
                                                      const SymbolOrigins& symbolOrigins) {
    environment_.activeModule = &module;
    environment_.symbolOrigins = symbolOrigins;
    session_.specializationRequests.clear();
    session_.structSpecializationRequests.clear();
    session_.scopes.clear();
    session_.currentFunctionName.clear();

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
      requireExplicitReturn(function);
    }

    for (std::size_t index = firstNewFunction; index < module.functions.size(); ++index) {
      checkFunction(module.functions[index]);
    }
  }

  void TypeChecker::checkFunction(const ast::Function& function) {
    session_.scopes.clear();
    pushScope();

    session_.currentFunctionName = function.name;

    const bool allowInternal = isStdlibContext();
    if (function.returnType != Type::voidType()) {
      requireKnownType(function.returnType, function.location, nullptr, false, allowInternal);
    }
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

  bool
  TypeChecker::checkStatements(const std::vector<std::unique_ptr<ast::Statement>>& statements,
                                     Type expectedReturnType) {
    bool returned = false;
    for (const auto& statement : statements) {
      returned = checkStatement(*statement, expectedReturnType) || returned;
    }

    return returned;
  }

  bool TypeChecker::checkStatement(const ast::Statement& statement, Type expectedReturnType) {
    const auto strategy = activate(TypeCheckerStrategyKind::Statements);
    StatementVisitor visitor(*this, expectedReturnType);
    statement.accept(visitor);
    return visitor.returned();
  }

  Type TypeChecker::checkRvalue(const ast::Expression& expression,
                                      std::optional<Type> expectedType) {
    const auto strategy = activate(TypeCheckerStrategyKind::Expressions);
    ExpressionVisitor visitor(*this, std::move(expectedType));
    expression.accept(visitor);
    return visitor.result();
  }

  bool TypeChecker::declareLocal(const std::string& name, Type type) {
    if (session_.scopes.empty())
      pushScope();

    auto& scope = session_.scopes.back();
    if (scope.contains(name))
      return false;

    scope.emplace(name, type);
    return true;
  }

  Type TypeChecker::lookupLocal(const std::string& name, SourceLocation location) const {
    for (auto scope = session_.scopes.rbegin(); scope != session_.scopes.rend(); ++scope) {
      const auto local = scope->find(name);

      if (local != scope->end())
        return local->second;
    }

    throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                        "unknown local variable '" + name + "'"));
  }

  void TypeChecker::pushScope() {
    session_.scopes.emplace_back();
  }

  void TypeChecker::popScope() {
    session_.scopes.pop_back();
  }

} // namespace noria
