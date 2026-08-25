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

namespace noria {

  using namespace typecheck_detail;

  TypeChecker::Impl::Impl() : relations_(*this) {}

  TypeChecker::Impl::Impl(const Impl& other)
      : environment_(other.environment_), session_(other.session_), relations_(*this) {}

  TypeChecker::Impl& TypeChecker::Impl::operator=(const Impl& other) {
    if (this != &other) {
      environment_ = other.environment_;
      session_ = other.session_;
    }
    return *this;
  }

  void TypeChecker::Impl::check(const ast::Module& module, const SymbolOrigins& symbolOrigins) {
    environment_.activeModule = &module;
    environment_.functions.clear();
    environment_.genericFunctions.clear();
    session_.specializationRequests.clear();
    session_.structSpecializationRequests.clear();
    session_.scopes.clear();
    session_.currentFunctionName.clear();
    environment_.symbolOrigins = symbolOrigins;

    collectStructDecls(module);
    collectFunctionSignatures(module);

    for (const auto& function : module.functions) {
      if (function.typeParams.empty()) {
        checkFunction(function);
      }
    }
  }

  void TypeChecker::Impl::checkSpecializationFrontier(const ast::Module& module,
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
    }

    for (std::size_t index = firstNewFunction; index < module.functions.size(); ++index) {
      checkFunction(module.functions[index]);
    }
  }

  void TypeChecker::Impl::checkFunction(const ast::Function& function) {
    session_.scopes.clear();
    pushScope();

    session_.currentFunctionName = function.name;

    const bool allowInternal = isStdlibContext();
    requireKnownType(function.returnType, function.location, nullptr, false, allowInternal);
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
  TypeChecker::Impl::checkStatements(const std::vector<std::unique_ptr<ast::Statement>>& statements,
                                     Type expectedReturnType) {
    bool returned = false;
    for (const auto& statement : statements) {
      returned = checkStatement(*statement, expectedReturnType) || returned;
    }

    return returned;
  }

  bool TypeChecker::Impl::checkStatement(const ast::Statement& statement, Type expectedReturnType) {
    StatementVisitor visitor(*this, expectedReturnType);
    statement.accept(visitor);
    return visitor.returned();
  }

  void TypeChecker::Impl::registerFunctionSpecialization(std::string mangledName,
                                                         std::vector<Type> typeArgs) {
    session_.functionSpecializationTypeArgs.emplace(std::move(mangledName), std::move(typeArgs));
  }

  std::vector<SpecializationRequest> TypeChecker::Impl::takeSpecializationRequests() {
    std::vector<SpecializationRequest> requests;
    requests.swap(session_.specializationRequests);
    return requests;
  }

  std::vector<StructSpecializationRequest>
  TypeChecker::Impl::takeStructSpecializationRequests() const {
    std::vector<StructSpecializationRequest> requests;
    requests.swap(session_.structSpecializationRequests);
    return requests;
  }

  Type TypeChecker::Impl::checkRvalue(const ast::Expression& expression,
                                      std::optional<Type> expectedType) {
    ExpressionVisitor visitor(*this, std::move(expectedType));
    expression.accept(visitor);
    return visitor.result();
  }

  bool TypeChecker::Impl::declareLocal(const std::string& name, Type type) {
    if (session_.scopes.empty())
      pushScope();

    auto& scope = session_.scopes.back();
    if (scope.contains(name))
      return false;

    scope.emplace(name, type);
    return true;
  }

  Type TypeChecker::Impl::lookupLocal(const std::string& name, SourceLocation location) const {
    for (auto scope = session_.scopes.rbegin(); scope != session_.scopes.rend(); ++scope) {
      const auto local = scope->find(name);

      if (local != scope->end())
        return local->second;
    }

    throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                        "unknown local variable '" + name + "'"));
  }

  void TypeChecker::Impl::pushScope() {
    session_.scopes.emplace_back();
  }

  void TypeChecker::Impl::popScope() {
    session_.scopes.pop_back();
  }

} // namespace noria
