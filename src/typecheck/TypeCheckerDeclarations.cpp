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

  DeclarationChecker::DeclarationChecker(TypeCheckContext& context, TypeRelations& relations)
      : TypeCheckComponent(context), relations_(relations) {}

  bool DeclarationChecker::isStdlibOrigin(const std::string& modulePath) const {
    return modulePath.rfind("std::", 0) == 0;
  }

  bool DeclarationChecker::isStdlibContext() const {
    const auto origin = environment().symbolOrigins.functions.find(session().currentFunctionName);
    if (origin == environment().symbolOrigins.functions.end()) {
      return false;
    }
    return isStdlibOrigin(origin->second);
  }

  bool DeclarationChecker::isInternalModuleOrigin(const std::string& modulePath) const {
    return modulePath.rfind("std::internal::", 0) == 0;
  }

  void DeclarationChecker::requireFunctionCallable(const std::string& calleeName,
                                            SourceLocation location) const {
    if (environment().symbolOrigins.hiddenFunctions.contains(calleeName)) {
      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                          "function '" + calleeName + "' is internal"));
    }

    const auto origin = environment().symbolOrigins.functions.find(calleeName);
    if (origin == environment().symbolOrigins.functions.end()) {
      return;
    }

    if (isInternalModuleOrigin(origin->second) && !isStdlibContext()) {
      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                          "function '" + calleeName + "' is internal to module '" +
                                              origin->second + "'"));
    }
  }

  const ast::Function& DeclarationChecker::genericFunctionAt(std::size_t moduleIndex) const {
    if (environment().activeModule == nullptr ||
        moduleIndex >= environment().activeModule->functions.size()) {
      throw CompileError("typecheck: internal error: invalid generic function index");
    }
    return environment().activeModule->functions[moduleIndex];
  }

  const ast::StructDecl& DeclarationChecker::genericStructAt(std::size_t moduleIndex) const {
    if (environment().activeModule == nullptr ||
        moduleIndex >= environment().activeModule->structs.size()) {
      throw CompileError("typecheck: internal error: invalid generic struct index");
    }
    return environment().activeModule->structs[moduleIndex];
  }

  void DeclarationChecker::collectFunctionSignatures(const ast::Module& module) {
    for (std::size_t index{}; index < module.functions.size(); ++index) {
      const ast::Function& function = module.functions[index];
      if (!function.typeParams.empty()) {
        collectGenericFunctionSignature(function, index);
        continue;
      }

      collectConcreteFunctionSignature(function);
    }

    for (const auto& [name, family] : environment().genericFunctions) {
      validateGenericFunctionFamily(name, family);
    }
  }

  void DeclarationChecker::requireDefinableFunctionName(const ast::Function& function) const {
    if (lookupBuiltin(function.name) != nullptr) {
      throw CompileError(
          formatDiagnostic(function.location, DiagnosticStage::TypeCheck,
                           "cannot define function '" + function.name + "': name is a builtin"));
    }

    if ((function.name.rfind("__rt_", 0) == 0 || function.name.rfind("__noria_", 0) == 0) &&
        !allowsInternalFunctionTypes(function)) {
      throw CompileError(formatDiagnostic(function.location, DiagnosticStage::TypeCheck,
                                          "name '" + function.name + "' is reserved"));
    }
  }

  void DeclarationChecker::collectGenericFunctionSignature(const ast::Function& function,
                                                    std::size_t moduleIndex) {
    requireDefinableFunctionName(function);
    const auto existing = environment().genericFunctions.find(function.name);
    if (existing != environment().genericFunctions.end()) {
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
    } else if (environment().functions.contains(function.name)) {
      throw CompileError(formatDiagnostic(function.location, DiagnosticStage::TypeCheck,
                                          "duplicate function '" + function.name + "'"));
    }

    const bool allowInternal = allowsInternalFunctionTypes(function);

    std::unordered_set<std::string> allowedTypeParams;
    for (const auto& typeParam : function.typeParams) {
      allowedTypeParams.insert(typeParam.name);
    }

    if (!function.returnType) {
      throw CompileError("typecheck: internal error: function '" + function.name +
                         "' has an unresolved return type");
    }
    if (*function.returnType != Type::voidType()) {
      relations_.requireKnownType(*function.returnType, function.location, &allowedTypeParams, false,
                                  allowInternal);
    }
    for (const auto& parameter : function.parameters) {
      relations_.requireKnownType(parameter.type, parameter.location, &allowedTypeParams, false,
                                  allowInternal);
    }

    environment().genericFunctions[function.name].push_back(moduleIndex);
  }

  void DeclarationChecker::collectConcreteFunctionSignature(const ast::Function& function) {
    requireDefinableFunctionName(function);
    if (environment().functions.contains(function.name) ||
        environment().genericFunctions.contains(function.name)) {
      throw CompileError(formatDiagnostic(function.location, DiagnosticStage::TypeCheck,
                                          "duplicate function '" + function.name + "'"));
    }

    const bool allowInternal = allowsInternalFunctionTypes(function);

    FunctionSignature signature;
    if (!function.returnType) {
      throw CompileError("typecheck: internal error: function '" + function.name +
                         "' has an unresolved return type");
    }
    if (*function.returnType != Type::voidType()) {
      relations_.requireKnownType(*function.returnType, function.location, nullptr, false,
                                  allowInternal);
    }
    signature.returnType = *function.returnType;

    for (const auto& parameter : function.parameters) {
      relations_.requireKnownType(parameter.type, parameter.location, nullptr, false,
                                  allowInternal);
      signature.parameterTypes.push_back(parameter.type);
    }

    environment().functions.emplace(function.name, std::move(signature));
  }

  void DeclarationChecker::validateGenericFunctionFamily(std::string_view name,
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

  bool DeclarationChecker::allowsInternalFunctionTypes(const ast::Function& function) const {
    const auto origin = environment().symbolOrigins.functions.find(function.name);
    return origin != environment().symbolOrigins.functions.end() && isStdlibOrigin(origin->second);
  }

} // namespace noria
