#include "MonomorphizeInternal.hpp"

#include "../internal/AstVisitorAdapters.hpp"
#include "../internal/SpecializationSupport.hpp"
#include "noria/AstClone.hpp"
#include "noria/AstVisitor.hpp"
#include "noria/CompilerCache.hpp"
#include "noria/Diagnostic.hpp"
#include "noria/ModuleResolver.hpp"
#include "noria/SemanticTables.hpp"
#include "noria/TypeChecker.hpp"

#include <algorithm>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace noria::monomorphize_detail {

  using internal::containsUnboundTypeParam;
  using internal::findImplTag;

  Type substituteType(const Type& type, const Substitution& substitution) {
    if (type.kind() == TypeKind::TypeParam) {
      const auto bound = substitution.find(type.typeParameterName());
      if (bound == substitution.end()) {
        throw CompileError("internal: unbound type parameter '" + type.typeParameterName() + "'");
      }
      return bound->second;
    }

    if (type.kind() == TypeKind::Array) {
      return Type::array(substituteType(type.elementType(), substitution));
    }

    if (type.kind() == TypeKind::Struct) {
      if (type.typeArguments().empty()) {
        return type;
      }
      std::vector<Type> substitutedArgs;
      substitutedArgs.reserve(type.typeArguments().size());
      for (const Type& typeArg : type.typeArguments()) {
        substitutedArgs.push_back(substituteType(typeArg, substitution));
      }
      return Type::structType(type.structName(), std::move(substitutedArgs));
    }

    return type;
  }

  Type rewriteAppliedStructType(const Type& type) {
    if (type.kind() == TypeKind::Struct && !type.typeArguments().empty()) {
      if (containsUnboundTypeParam(type)) {
        return type;
      }
      return Type::structType(mangleSpecialization(type.structName(), type.typeArguments()), {});
    }

    if (type.kind() == TypeKind::Array) {
      return Type::array(rewriteAppliedStructType(type.elementType()));
    }

    return type;
  }

  Substitution bindTypeParameters(const std::vector<ast::TypeParameter>& typeParams,
                                  const std::vector<Type>& typeArgs, std::string_view context) {
    if (typeArgs.size() != typeParams.size()) {
      throw CompileError("internal: " + std::string(context) + " type argument count mismatch");
    }

    Substitution substitution;
    for (std::size_t index{}; index < typeParams.size(); ++index) {
      substitution.emplace(typeParams[index].name, typeArgs[index]);
    }
    return substitution;
  }

} // namespace noria::monomorphize_detail

namespace noria {

  using namespace monomorphize_detail;

  Type substitute(const Type& type, const Substitution& substitution) {
    return substituteType(type, substitution);
  }

  Type substituteSpecializationType(const Type& type, const Substitution& substitution) {
    return rewriteAppliedStructType(substituteType(type, substitution));
  }

  std::string mangleType(const Type& type) {
    if (type.kind() == TypeKind::Struct) {
      if (type.typeArguments().empty()) {
        return "st." + type.structName();
      }
      {
        std::ostringstream out;
        out << "st." << type.structName();
        for (const Type& typeArg : type.typeArguments()) {
          out << "$" << mangleType(typeArg);
        }
        return out.str();
      }
    }

    if (type.kind() == TypeKind::Array) {
      return "arr." + mangleType(type.elementType());
    }

    if (type.kind() == TypeKind::ImplTag) {
      return "tag." + std::string(implementationTagName(type.implementationTagValue()));
    }

    if (type.kind() == TypeKind::TypeParam) {
      throw CompileError("internal: cannot mangle unsubstituted type parameter '" +
                         type.typeParameterName() + "'");
    }

    if (const TypeKindInfo* info = typeKindInfo(type.kind()); info && !info->mangleAtom.empty()) {
      return std::string(info->mangleAtom);
    }
    return "unknown";
  }

  std::string mangleSpecialization(std::string_view templateName,
                                   const std::vector<Type>& typeArgs) {
    std::ostringstream out;
    out << templateName;
    for (const Type& typeArg : typeArgs) {
      out << '$' << mangleType(typeArg);
    }
    return out.str();
  }

} // namespace noria
