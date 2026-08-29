#include "TypeCheckerInternal.hpp"

#include "noria/Diagnostic.hpp"

#include <utility>

namespace noria::typecheck_detail {

  bool ScopeStack::declare(const std::string& name, Type type) {
    if (scopes_.empty()) {
      push();
    }

    Scope& scope = scopes_.back();
    if (scope.contains(name)) {
      return false;
    }
    scope.emplace(name, std::move(type));
    return true;
  }

  Type ScopeStack::lookup(const std::string& name, SourceLocation location) const {
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
      const auto local = scope->find(name);
      if (local != scope->end()) {
        return local->second;
      }
    }
    throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                        "unknown local variable '" + name + "'"));
  }

  void SpecializationRegistry::registerFunction(std::string name, std::vector<Type> typeArgs) {
    functionTypeArgs_.emplace(std::move(name), std::move(typeArgs));
  }

  void SpecializationRegistry::registerStruct(std::string name, std::vector<Type> typeArgs) {
    structTypeArgs_.emplace(std::move(name), std::move(typeArgs));
  }

  const std::vector<SpecializationRequest>& SpecializationRegistry::functionRequests() const {
    return functionRequests_;
  }

  const std::vector<StructSpecializationRequest>& SpecializationRegistry::structRequests() const {
    return structRequests_;
  }

  void SpecializationRegistry::clearRequests() {
    functionRequests_.clear();
    structRequests_.clear();
  }

  void SpecializationRegistry::clearFunctionRequests() { functionRequests_.clear(); }

  void SpecializationRegistry::clearStructRequests() { structRequests_.clear(); }

  std::vector<SpecializationRequest> SpecializationRegistry::takeFunctionRequests() {
    std::vector<SpecializationRequest> requests;
    requests.swap(functionRequests_);
    return requests;
  }

  std::vector<StructSpecializationRequest> SpecializationRegistry::takeStructRequests() const {
    std::vector<StructSpecializationRequest> requests;
    requests.swap(structRequests_);
    return requests;
  }

  void SpecializationRegistry::recordFunctionRequest(SpecializationRequest request) {
    functionRequests_.push_back(std::move(request));
  }

  void SpecializationRegistry::recordStructRequest(StructSpecializationRequest request) {
    structRequests_.push_back(std::move(request));
  }

  const std::unordered_map<std::string, std::vector<Type>>&
  SpecializationRegistry::functionTypeArgs() const {
    return functionTypeArgs_;
  }

  const std::unordered_map<std::string, std::vector<Type>>&
  SpecializationRegistry::structTypeArgs() const {
    return structTypeArgs_;
  }

} // namespace noria::typecheck_detail
