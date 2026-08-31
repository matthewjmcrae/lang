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

  namespace typecheck_detail {

    bool isScalarWitnessType(const Type& type) {
      return type == Type::i32() || type == Type::f64() || type == Type::boolean() ||
             type == Type::str();
    }

    bool structSpecializationsMatch(const Type& left, const Type& right) {
      if (left.kind() != TypeKind::Struct || right.kind() != TypeKind::Struct) {
        return false;
      }

      if (left.structName() == right.structName() && left.typeArguments() == right.typeArguments()) {
        return true;
      }

      if (left.typeArguments().empty() && !right.typeArguments().empty()) {
        if (containsUnboundTypeParam(right)) {
          return false;
        }
        return left.structName() == mangleSpecialization(right.structName(), right.typeArguments());
      }

      if (right.typeArguments().empty() && !left.typeArguments().empty()) {
        if (containsUnboundTypeParam(left)) {
          return false;
        }
        return right.structName() == mangleSpecialization(left.structName(), left.typeArguments());
      }

      return false;
    }

    bool allTypeParamsSubstituted(const Type& type, const Substitution& substitution) {
      if (type.kind() == TypeKind::TypeParam) {
        return substitution.contains(type.typeParameterName());
      }

      if (type.kind() == TypeKind::Array) {
        return allTypeParamsSubstituted(type.elementType(), substitution);
      }

      if (type.kind() == TypeKind::Struct) {
        for (const Type& typeArg : type.typeArguments()) {
          if (!allTypeParamsSubstituted(typeArg, substitution)) {
            return false;
          }
        }
      }

      return true;
    }

    bool sameGenericPublicApi(const ast::Function& left, const ast::Function& right) {
      if (left.typeParams.size() != right.typeParams.size()) {
        return false;
      }
      for (std::size_t index{}; index < left.typeParams.size(); ++index) {
        if (left.typeParams[index].name != right.typeParams[index].name) {
          return false;
        }
      }
      if (left.parameters.size() != right.parameters.size()) {
        return false;
      }
      for (std::size_t index{}; index < left.parameters.size(); ++index) {
        if (left.parameters[index].type != right.parameters[index].type) {
          return false;
        }
      }
      return left.returnType == right.returnType;
    }

    void rejectStructArrayElement(const Type& elementType, SourceLocation location) {
      if (elementType.kind() == TypeKind::Struct) {
        throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                            "array element type cannot be a struct"));
      }
    }

    const ast::Function* selectGenericImplementation(const ast::Module& module,
                                                     const std::vector<std::size_t>& family,
                                                     std::optional<ImplementationTag> callTag,
                                                     std::string_view functionName,
                                                     SourceLocation location) {
      if (family.empty()) {
        return nullptr;
      }

      if (family.size() == 1 && !module.functions.at(family.front()).implTag) {
        return &module.functions.at(family.front());
      }

      if (!callTag) {
        throw CompileError(
            formatDiagnostic(location, DiagnosticStage::TypeCheck,
                             "cannot select implementation of '" + std::string(functionName) +
                                 "' without an implementation tag in inferred type arguments"));
      }

      for (std::size_t candidateIndex : family) {
        const ast::Function& candidate = module.functions.at(candidateIndex);
        if (candidate.implTag && *candidate.implTag == *callTag) {
          return &candidate;
        }
      }

      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                          "no implementation of '" + std::string(functionName) +
                                              "' for tag '" +
                                              std::string(implementationTagName(*callTag)) + "'"));
    }

  } // namespace typecheck_detail

} // namespace noria
