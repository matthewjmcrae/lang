#include "TypeCheckerInternal.hpp"

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

  TypeCheckDriver::TypeCheckDriver(TypeCheckContext& context, TypeRelations& relations,
                                   DeclarationChecker& declarations, ExpressionChecker& expressions,
                                   StatementChecker& statements, StructChecker& structs)
      : TypeCheckComponent(context), relations_(relations), declarations_(declarations),
        expressions_(expressions), statements_(statements), structs_(structs) {}

  void TypeCheckDriver::resetForCheck(ast::Module& module, const SymbolOrigins& symbolOrigins) {
    environment().activeModule = &module;
    environment().functions.clear();
    environment().genericFunctions.clear();
    specializations().clearRequests();
    scopes().clear();
    session().currentFunctionName.clear();
    pendingReturnTypeFunctions_.clear();
    inferringReturnTypes_ = false;
    environment().symbolOrigins = symbolOrigins;
  }

  void TypeCheckDriver::check(ast::Module& module, const SymbolOrigins& symbolOrigins) {
    resetForCheck(module, symbolOrigins);

    for (const auto& function : module.functions) {
      requireExplicitReturn(function);
    }

    structs_.collectStructDecls(module);
    inferFunctionReturnTypes(module);

    for (const auto& function : module.functions) {
      if (!function.returnType) {
        throw CompileError("typecheck: internal error: function '" + function.name +
                           "' has an unresolved return type");
      }
      requireReturnForms(function.body, *function.returnType);
    }
    declarations_.collectFunctionSignatures(module);

    for (const auto& function : module.functions) {
      if (function.typeParams.empty()) {
        checkFunction(function);
      }
    }
  }

  void TypeCheckDriver::mergeInferredReturnType(ReturnInferenceResult& result, Type type,
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
    if (!relations_.isAssignable(*result.type, type) || !relations_.isAssignable(type, *result.type)) {
      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                          "return type " + type.name() +
                                              " does not match expected " +
                                              result.type->name()));
    }
  }

  void TypeCheckDriver::inferReturnTypesInStatements(
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
              result, expressions_.checkRvalue(*returnStatement->expression, result.type),
              returnStatement->expression->location);
        } catch (const ReturnInferencePending&) {
          result.sawPendingCall = true;
        }
        continue;
      }

      if (const auto* letStatement = dynamic_cast<const ast::LetStatement*>(statement.get())) {
        if (letStatement->declaredType) {
          scopes().declare(letStatement->name, *letStatement->declaredType);
          continue;
        }
        if (!letStatement->initializer) {
          continue;
        }
        try {
          const Type localType = expressions_.checkRvalue(*letStatement->initializer);
          if (localType != Type::voidType()) {
            scopes().declare(letStatement->name, localType);
          }
        } catch (const ReturnInferencePending&) {
          result.sawPendingCall = true;
        }
        continue;
      }

      if (const auto* ifStatement = dynamic_cast<const ast::IfStatement*>(statement.get())) {
        {
          auto scope = scopes().frame();
          inferReturnTypesInStatements(ifStatement->thenBranch, result);
        }
        {
          auto scope = scopes().frame();
          inferReturnTypesInStatements(ifStatement->elseBranch, result);
        }
        continue;
      }

      if (const auto* whileStatement = dynamic_cast<const ast::WhileStatement*>(statement.get())) {
        auto scope = scopes().frame();
        inferReturnTypesInStatements(whileStatement->body, result);
      }
    }
  }

  std::optional<Type> TypeCheckDriver::inferFunctionReturnType(const ast::Function& function) {
    scopes().clear();
    session().currentFunctionName = function.name;
    auto scope = scopes().frame();
    for (const auto& parameter : function.parameters) {
      scopes().declare(parameter.name, parameter.type);
    }

    ReturnInferenceResult result;
    inferReturnTypesInStatements(function.body, result);
    return result.type;
  }

  void TypeCheckDriver::inferFunctionReturnTypes(ast::Module& module) {
    pendingReturnTypeFunctions_.clear();
    for (const ast::Function& function : module.functions) {
      if (!function.returnType) {
        pendingReturnTypeFunctions_.insert(function.name);
      }
    }
    if (pendingReturnTypeFunctions_.empty()) {
      return;
    }

    // Only fully known function families are callable during inference. This lets each completed
    // family become available to forward callers while keeping incomplete recursive families pending.
    for (std::size_t index{}; index < module.functions.size(); ++index) {
      const ast::Function& function = module.functions[index];
      if (pendingReturnTypeFunctions_.contains(function.name)) {
        continue;
      }
      if (function.typeParams.empty()) {
        declarations_.collectConcreteFunctionSignature(function);
      } else {
        declarations_.collectGenericFunctionSignature(function, index);
      }
    }

    inferringReturnTypes_ = true;
    while (!pendingReturnTypeFunctions_.empty()) {
      bool madeProgress = false;
      std::vector<std::string> pendingNames(pendingReturnTypeFunctions_.begin(),
                                            pendingReturnTypeFunctions_.end());
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
        pendingReturnTypeFunctions_.erase(name);

        for (std::size_t index{}; index < module.functions.size(); ++index) {
          const ast::Function& function = module.functions[index];
          if (function.name != name) {
            continue;
          }
          if (function.typeParams.empty()) {
            declarations_.collectConcreteFunctionSignature(function);
          } else {
            declarations_.collectGenericFunctionSignature(function, index);
          }
        }
        madeProgress = true;
      }

      if (madeProgress) {
        continue;
      }

      const std::string& name = *pendingReturnTypeFunctions_.begin();
      const auto function = std::find_if(module.functions.begin(), module.functions.end(),
                                         [&](const ast::Function& candidate) {
                                           return candidate.name == name && !candidate.returnType;
                                         });
      inferringReturnTypes_ = false;
      throw CompileError(formatDiagnostic(
          function->location, DiagnosticStage::TypeCheck,
          "cannot infer return type for function '" + name +
              "'; add an explicit '-> Type'"));
    }
    inferringReturnTypes_ = false;
    specializations().clearRequests();
    scopes().clear();
    session().currentFunctionName.clear();
    environment().functions.clear();
    environment().genericFunctions.clear();
  }

  void TypeCheckDriver::checkSpecializationFrontier(const ast::Module& module,
                                                      std::size_t firstNewStruct,
                                                      std::size_t firstNewFunction,
                                                      const SymbolOrigins& symbolOrigins) {
    environment().activeModule = &module;
    environment().symbolOrigins = symbolOrigins;
    specializations().clearRequests();
    scopes().clear();
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
      structs_.collectConcreteStructDecl(decl);
    }
    structs_.validateConcreteStructFieldTypes(module, firstNewStruct);

    for (std::size_t index = firstNewFunction; index < module.functions.size(); ++index) {
      const ast::Function& function = module.functions[index];
      if (!function.typeParams.empty()) {
        throw CompileError(
            "typecheck: internal error: specialization frontier contains generic function");
      }
      declarations_.collectConcreteFunctionSignature(function);
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

  void TypeCheckDriver::checkFunction(const ast::Function& function) {
    scopes().clear();
    auto scope = scopes().frame();

    session().currentFunctionName = function.name;

    if (!function.returnType) {
      throw CompileError("typecheck: internal error: function '" + function.name +
                         "' has an unresolved return type");
    }

    const bool allowInternal = declarations_.isStdlibContext();
    if (*function.returnType != Type::voidType()) {
      relations_.requireKnownType(*function.returnType, function.location, nullptr, false,
                                  allowInternal);
    }
    const Type expectedReturnType = *function.returnType;
    if (expectedReturnType != Type::voidType()) {
      relations_.requireContainerOwnershipOps(expectedReturnType, function.location);
    }

    for (const auto& parameter : function.parameters) {
      relations_.requireKnownType(parameter.type, parameter.location, nullptr, false, allowInternal);
      const Type parameterType = parameter.type;

      if (!scopes().declare(parameter.name, parameterType)) {
        throw CompileError(formatDiagnostic(parameter.location, DiagnosticStage::TypeCheck,
                                            "duplicate parameter '" + parameter.name + "'"));
      }
      relations_.requireContainerOwnershipOps(parameterType, parameter.location);
    }

    statements_.checkStatements(function.body, expectedReturnType);
  }

  bool
  StatementChecker::checkStatements(const std::vector<std::unique_ptr<ast::Statement>>& statements,
                                     Type expectedReturnType) {
    bool returned = false;
    for (const auto& statement : statements) {
      returned = checkStatement(*statement, expectedReturnType) || returned;
    }

    return returned;
  }

  bool StatementChecker::checkStatement(const ast::Statement& statement, Type expectedReturnType) {
    StatementVisitor visitor(*this, expectedReturnType);
    statement.accept(visitor);
    return visitor.returned();
  }

  Type ExpressionChecker::checkRvalue(const ast::Expression& expression,
                                      std::optional<Type> expectedType) {
    ExpressionVisitor visitor(*this, std::move(expectedType));
    expression.accept(visitor);
    return visitor.result();
  }

} // namespace noria
