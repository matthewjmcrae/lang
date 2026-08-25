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

  bool TypeChecker::Impl::isStdlibOrigin(const std::string& modulePath) const {
    return modulePath.rfind("std::", 0) == 0;
  }

  bool TypeChecker::Impl::isStdlibContext() const {
    const auto origin = environment_.symbolOrigins.functions.find(session_.currentFunctionName);
    if (origin == environment_.symbolOrigins.functions.end()) {
      return false;
    }
    return isStdlibOrigin(origin->second);
  }

  bool TypeChecker::Impl::isInternalModuleOrigin(const std::string& modulePath) const {
    return modulePath.rfind("std::internal::", 0) == 0;
  }

  void TypeChecker::Impl::requireFunctionCallable(const std::string& calleeName,
                                                  SourceLocation location) const {
    const auto origin = environment_.symbolOrigins.functions.find(calleeName);
    if (origin == environment_.symbolOrigins.functions.end()) {
      return;
    }

    if (isInternalModuleOrigin(origin->second) && !isStdlibContext()) {
      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                          "function '" + calleeName + "' is internal to module '" +
                                              origin->second + "'"));
    }
  }

  const ast::Function& TypeChecker::Impl::genericFunctionAt(std::size_t moduleIndex) const {
    if (environment_.activeModule == nullptr ||
        moduleIndex >= environment_.activeModule->functions.size()) {
      throw CompileError("typecheck: internal error: invalid generic function index");
    }
    return environment_.activeModule->functions[moduleIndex];
  }

  const ast::StructDecl& TypeChecker::Impl::genericStructAt(std::size_t moduleIndex) const {
    if (environment_.activeModule == nullptr ||
        moduleIndex >= environment_.activeModule->structs.size()) {
      throw CompileError("typecheck: internal error: invalid generic struct index");
    }
    return environment_.activeModule->structs[moduleIndex];
  }

  void TypeChecker::Impl::collectFunctionSignatures(const ast::Module& module) {
    for (std::size_t index{}; index < module.functions.size(); ++index) {
      const ast::Function& function = module.functions[index];
      if (!function.typeParams.empty()) {
        collectGenericFunctionSignature(function, index);
        continue;
      }

      collectConcreteFunctionSignature(function);
    }

    for (const auto& [name, family] : environment_.genericFunctions) {
      validateGenericFunctionFamily(name, family);
    }
  }

  void TypeChecker::Impl::collectGenericFunctionSignature(const ast::Function& function,
                                                          std::size_t moduleIndex) {
    const auto existing = environment_.genericFunctions.find(function.name);
    if (existing != environment_.genericFunctions.end()) {
      for (std::size_t candidateIndex : existing->second) {
        const ast::Function& candidate = genericFunctionAt(candidateIndex);
        if (function.implTag && candidate.implTag && *function.implTag == *candidate.implTag) {
          throw CompileError(formatDiagnostic(
              function.location, DiagnosticStage::TypeCheck,
              "duplicate implementation '" + std::string(implementationTagName(*function.implTag)) +
                  "' for generic function '" + function.name + "'"));
        }
        if (static_cast<bool>(function.implTag) != static_cast<bool>(candidate.implTag)) {
          throw CompileError(formatDiagnostic(function.location, DiagnosticStage::TypeCheck,
                                              "generic function '" + function.name +
                                                  "' mixes tagged and untagged implementations"));
        }
        if (!function.implTag && !candidate.implTag) {
          throw CompileError(formatDiagnostic(function.location, DiagnosticStage::TypeCheck,
                                              "duplicate function '" + function.name + "'"));
        }
      }
    } else if (environment_.functions.contains(function.name)) {
      throw CompileError(formatDiagnostic(function.location, DiagnosticStage::TypeCheck,
                                          "duplicate function '" + function.name + "'"));
    }

    const bool allowInternal = allowsInternalFunctionTypes(function);
    if (function.name.rfind("__rt_", 0) == 0 && !allowInternal) {
      throw CompileError(formatDiagnostic(function.location, DiagnosticStage::TypeCheck,
                                          "name '" + function.name + "' is reserved"));
    }

    std::unordered_set<std::string> allowedTypeParams;
    for (const auto& typeParam : function.typeParams) {
      allowedTypeParams.insert(typeParam.name);
    }

    requireKnownType(function.returnType, function.location, &allowedTypeParams, false,
                     allowInternal);
    for (const auto& parameter : function.parameters) {
      requireKnownType(parameter.type, parameter.location, &allowedTypeParams, false,
                       allowInternal);
    }

    environment_.genericFunctions[function.name].push_back(moduleIndex);
  }

  void TypeChecker::Impl::collectConcreteFunctionSignature(const ast::Function& function) {
    if (environment_.functions.contains(function.name) ||
        environment_.genericFunctions.contains(function.name)) {
      throw CompileError(formatDiagnostic(function.location, DiagnosticStage::TypeCheck,
                                          "duplicate function '" + function.name + "'"));
    }

    const bool allowInternal = allowsInternalFunctionTypes(function);
    if (function.name.rfind("__rt_", 0) == 0 && !allowInternal) {
      throw CompileError(formatDiagnostic(function.location, DiagnosticStage::TypeCheck,
                                          "name '" + function.name + "' is reserved"));
    }

    FunctionSignature signature;
    requireKnownType(function.returnType, function.location, nullptr, false, allowInternal);
    signature.returnType = function.returnType;

    for (const auto& parameter : function.parameters) {
      requireKnownType(parameter.type, parameter.location, nullptr, false, allowInternal);
      signature.parameterTypes.push_back(parameter.type);
    }

    environment_.functions.emplace(function.name, std::move(signature));
  }

  void
  TypeChecker::Impl::validateGenericFunctionFamily(std::string_view name,
                                                   const std::vector<std::size_t>& family) const {
    if (family.size() <= 1) {
      return;
    }

    const ast::Function& reference = genericFunctionAt(family.front());
    for (std::size_t index = 1; index < family.size(); ++index) {
      const ast::Function& candidate = genericFunctionAt(family[index]);
      if (!sameGenericPublicApi(reference, candidate)) {
        throw CompileError(formatDiagnostic(candidate.location, DiagnosticStage::TypeCheck,
                                            "implementation signature of '" + std::string(name) +
                                                "' does not match other implementations"));
      }
    }
  }

  bool TypeChecker::Impl::allowsInternalFunctionTypes(const ast::Function& function) const {
    const auto origin = environment_.symbolOrigins.functions.find(function.name);
    return origin != environment_.symbolOrigins.functions.end() && isStdlibOrigin(origin->second);
  }

} // namespace noria
