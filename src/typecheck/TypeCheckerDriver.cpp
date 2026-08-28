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
#include <utility>

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
      if (statementsAlwaysReturn(function.body)) {
        return;
      }
      throw CompileError(formatDiagnostic(
          function.location, DiagnosticStage::TypeCheck,
          "not all control-flow paths in function '" + function.name + "' return"));
    }

  } // namespace

  void TypeChecker::DriverState::check(ast::Module& module, const SymbolOrigins& symbolOrigins) {
    environment().activeModule = &module;
    environment().functions.clear();
    environment().genericFunctions.clear();
    session().specializationRequests.clear();
    session().structSpecializationRequests.clear();
    session().scopes.clear();
    session().currentFunctionName.clear();
    environment().symbolOrigins = symbolOrigins;

    for (const auto& function : module.functions) {
      requireExplicitReturn(function);
    }

    collectStructDecls(module);
    inferFunctionReturnTypes(module);

    for (const auto& function : module.functions) {
      if (!function.returnType) {
        throw CompileError("typecheck: internal error: function '" + function.name +
                           "' has an unresolved return type");
      }
      requireReturnForms(function.body, *function.returnType);
    }
    collectFunctionSignatures(module);

    for (const auto& function : module.functions) {
      if (function.typeParams.empty()) {
        checkFunction(function);
      }
    }
  }

  void TypeChecker::DriverState::mergeInferredReturnType(ReturnInferenceResult& result, Type type,
                                            SourceLocation location) {
    if (!result.type) {
      result.type = std::move(type);
      return;
    }

    if (*result.type == Type::voidType() && type != Type::voidType()) {
      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                          "void function cannot return a value"));
    }
    if (*result.type != Type::voidType() && type == Type::voidType()) {
      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                          "non-void function must return a value"));
    }
    if (!isAssignable(*result.type, type) || !isAssignable(type, *result.type)) {
      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                          "return type " + type.name() +
                                              " does not match expected " +
                                              result.type->name()));
    }
  }

  void TypeChecker::DriverState::inferReturnTypesInStatements(
      const std::vector<std::unique_ptr<ast::Statement>>& statements,
      ReturnInferenceResult& result) {
    for (const auto& statement : statements) {
      if (const auto* returnStatement = dynamic_cast<const ast::ReturnStatement*>(statement.get())) {
        if (!returnStatement->expression) {
          mergeInferredReturnType(result, Type::voidType(), returnStatement->location);
          continue;
        }

        try {
          mergeInferredReturnType(
              result, checkRvalue(*returnStatement->expression, result.type),
              returnStatement->expression->location);
        } catch (const ReturnInferencePending&) {
          result.sawPendingCall = true;
        }
        continue;
      }

      if (const auto* letStatement = dynamic_cast<const ast::LetStatement*>(statement.get())) {
        if (letStatement->declaredType) {
          declareLocal(letStatement->name, *letStatement->declaredType);
          continue;
        }
        if (!letStatement->initializer) {
          continue;
        }
        try {
          const Type localType = checkRvalue(*letStatement->initializer);
          if (localType != Type::voidType()) {
            declareLocal(letStatement->name, localType);
          }
        } catch (const ReturnInferencePending&) {
          result.sawPendingCall = true;
        }
        continue;
      }

      if (const auto* ifStatement = dynamic_cast<const ast::IfStatement*>(statement.get())) {
        pushScope();
        inferReturnTypesInStatements(ifStatement->thenBranch, result);
        popScope();
        pushScope();
        inferReturnTypesInStatements(ifStatement->elseBranch, result);
        popScope();
        continue;
      }

      if (const auto* whileStatement = dynamic_cast<const ast::WhileStatement*>(statement.get())) {
        pushScope();
        inferReturnTypesInStatements(whileStatement->body, result);
        popScope();
      }
    }
  }

  std::optional<Type> TypeChecker::DriverState::inferFunctionReturnType(const ast::Function& function) {
    session().scopes.clear();
    session().currentFunctionName = function.name;
    pushScope();
    for (const auto& parameter : function.parameters) {
      declareLocal(parameter.name, parameter.type);
    }

    ReturnInferenceResult result;
    inferReturnTypesInStatements(function.body, result);
    popScope();
    return result.type;
  }

  void TypeChecker::DriverState::inferFunctionReturnTypes(ast::Module& module) {
    pendingReturnTypeFunctions().clear();
    for (const ast::Function& function : module.functions) {
      if (!function.returnType) {
        pendingReturnTypeFunctions().insert(function.name);
      }
    }
    if (pendingReturnTypeFunctions().empty()) {
      return;
    }

    // Only fully known function families are callable during inference. This lets each completed
    // family become available to forward callers while keeping incomplete recursive families pending.
    for (std::size_t index{}; index < module.functions.size(); ++index) {
      const ast::Function& function = module.functions[index];
      if (pendingReturnTypeFunctions().contains(function.name)) {
        continue;
      }
      if (function.typeParams.empty()) {
        collectConcreteFunctionSignature(function);
      } else {
        collectGenericFunctionSignature(function, index);
      }
    }

    inferringReturnTypes() = true;
    while (!pendingReturnTypeFunctions().empty()) {
      bool madeProgress = false;
      std::vector<std::string> pendingNames(pendingReturnTypeFunctions().begin(),
                                            pendingReturnTypeFunctions().end());
      for (const std::string& name : pendingNames) {
        bool allResolved = true;
        std::vector<std::pair<std::size_t, Type>> inferred;
        for (std::size_t index{}; index < module.functions.size(); ++index) {
          ast::Function& function = module.functions[index];
          if (function.name != name || function.returnType) {
            continue;
          }
          const std::optional<Type> inferredType = inferFunctionReturnType(function);
          if (!inferredType) {
            allResolved = false;
            break;
          }
          inferred.emplace_back(index, *inferredType);
        }

        if (!allResolved) {
          continue;
        }

        for (const auto& [index, inferredType] : inferred) {
          module.functions[index].returnType = inferredType;
        }
        pendingReturnTypeFunctions().erase(name);

        for (std::size_t index{}; index < module.functions.size(); ++index) {
          const ast::Function& function = module.functions[index];
          if (function.name != name) {
            continue;
          }
          if (function.typeParams.empty()) {
            collectConcreteFunctionSignature(function);
          } else {
            collectGenericFunctionSignature(function, index);
          }
        }
        madeProgress = true;
      }

      if (madeProgress) {
        continue;
      }

      const std::string& name = *pendingReturnTypeFunctions().begin();
      const auto function = std::find_if(module.functions.begin(), module.functions.end(),
                                         [&](const ast::Function& candidate) {
                                           return candidate.name == name && !candidate.returnType;
                                         });
      inferringReturnTypes() = false;
      throw CompileError(formatDiagnostic(
          function->location, DiagnosticStage::TypeCheck,
          "cannot infer return type for function '" + name +
              "'; add an explicit '-> Type'"));
    }
    inferringReturnTypes() = false;
    session().specializationRequests.clear();
    session().structSpecializationRequests.clear();
    session().scopes.clear();
    session().currentFunctionName.clear();
    environment().functions.clear();
    environment().genericFunctions.clear();
  }

  void TypeChecker::DriverState::checkSpecializationFrontier(const ast::Module& module,
                                                      std::size_t firstNewStruct,
                                                      std::size_t firstNewFunction,
                                                      const SymbolOrigins& symbolOrigins) {
    environment().activeModule = &module;
    environment().symbolOrigins = symbolOrigins;
    session().specializationRequests.clear();
    session().structSpecializationRequests.clear();
    session().scopes.clear();
    session().currentFunctionName.clear();

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
      if (!function.returnType) {
        throw CompileError("typecheck: internal error: specialization has an unresolved return type");
      }
      requireReturnForms(function.body, *function.returnType);
      requireExplicitReturn(function);
    }

    for (std::size_t index = firstNewFunction; index < module.functions.size(); ++index) {
      checkFunction(module.functions[index]);
    }
  }

  void TypeChecker::DriverState::checkFunction(const ast::Function& function) {
    session().scopes.clear();
    pushScope();

    session().currentFunctionName = function.name;

    if (!function.returnType) {
      throw CompileError("typecheck: internal error: function '" + function.name +
                         "' has an unresolved return type");
    }

    const bool allowInternal = isStdlibContext();
    if (*function.returnType != Type::voidType()) {
      requireKnownType(*function.returnType, function.location, nullptr, false, allowInternal);
    }
    const Type expectedReturnType = *function.returnType;
    if (expectedReturnType != Type::voidType()) {
      requireContainerOwnershipOps(expectedReturnType, function.location);
    }

    for (const auto& parameter : function.parameters) {
      requireKnownType(parameter.type, parameter.location, nullptr, false, allowInternal);
      const Type parameterType = parameter.type;

      if (!declareLocal(parameter.name, parameterType)) {
        throw CompileError(formatDiagnostic(parameter.location, DiagnosticStage::TypeCheck,
                                            "duplicate parameter '" + parameter.name + "'"));
      }
      requireContainerOwnershipOps(parameterType, parameter.location);
    }

    checkStatements(function.body, expectedReturnType);
    popScope();
  }

  bool
  TypeChecker::StatementsState::checkStatements(const std::vector<std::unique_ptr<ast::Statement>>& statements,
                                     Type expectedReturnType) {
    bool returned = false;
    for (const auto& statement : statements) {
      returned = checkStatement(*statement, expectedReturnType) || returned;
    }

    return returned;
  }

  bool TypeChecker::StatementsState::checkStatement(const ast::Statement& statement, Type expectedReturnType) {
    StatementVisitor visitor(*this, expectedReturnType);
    statement.accept(visitor);
    return visitor.returned();
  }

  Type TypeChecker::ExpressionsState::checkRvalue(const ast::Expression& expression,
                                      std::optional<Type> expectedType) {
    ExpressionVisitor visitor(*this, std::move(expectedType));
    expression.accept(visitor);
    return visitor.result();
  }

  bool TypeChecker::StatementsState::declareLocal(const std::string& name, Type type) {
    if (session().scopes.empty())
      pushScope();

    auto& scope = session().scopes.back();
    if (scope.contains(name))
      return false;

    scope.emplace(name, type);
    return true;
  }

  Type TypeChecker::StatementsState::lookupLocal(const std::string& name, SourceLocation location) const {
    for (auto scope = session().scopes.rbegin(); scope != session().scopes.rend(); ++scope) {
      const auto local = scope->find(name);

      if (local != scope->end())
        return local->second;
    }

    throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                        "unknown local variable '" + name + "'"));
  }

  void TypeChecker::StatementsState::pushScope() {
    session().scopes.emplace_back();
  }

  void TypeChecker::StatementsState::popScope() {
    session().scopes.pop_back();
  }

} // namespace noria
