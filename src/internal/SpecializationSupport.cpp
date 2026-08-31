#include "SpecializationSupport.hpp"

namespace noria::internal {

  std::optional<Type> firstNonImplTagTypeArg(const std::vector<Type>& typeArgs) {
    for (const Type& typeArg : typeArgs) {
      if (typeArg.kind() != TypeKind::ImplTag) {
        return typeArg;
      }
    }
    return std::nullopt;
  }

  std::optional<ImplementationTag> findImplTag(const std::vector<Type>& typeArgs) {
    for (const Type& typeArg : typeArgs) {
      if (typeArg.kind() == TypeKind::ImplTag) {
        return typeArg.implementationTagValue();
      }
    }
    return std::nullopt;
  }

  bool containsUnboundTypeParam(const Type& type) {
    if (type.kind() == TypeKind::TypeParam) {
      return true;
    }
    if (type.kind() == TypeKind::Array) {
      return containsUnboundTypeParam(type.elementType());
    }
    if (type.kind() == TypeKind::Struct) {
      for (const Type& typeArg : type.typeArguments()) {
        if (containsUnboundTypeParam(typeArg)) {
          return true;
        }
      }
    }
    return false;
  }

} // namespace noria::internal
