#include "noria/Types.hpp"

#include "noria/Diagnostic.hpp"

#include <utility>

namespace noria {

  Type Type::array(Type elementType) {
    Type type(TypeKind::Array);
    type.element = std::make_shared<Type>(std::move(elementType));
    return type;
  }

  Type Type::structType(std::string name) {
    Type type(TypeKind::Struct);
    type.structName = std::move(name);
    return type;
  }

  Type Type::typeParam(std::string name) {
    Type type(TypeKind::TypeParam);
    type.typeParamName = std::move(name);
    return type;
  }

  bool Type::operator==(const Type& other) const {
    if (kind != other.kind)
      return false;

    switch (kind) {
    case TypeKind::Array:
      if (!element || !other.element)
        return element == other.element;
      return *element == *other.element;
    case TypeKind::Struct:
      return structName == other.structName;
    case TypeKind::TypeParam:
      return typeParamName == other.typeParamName;
    default:
      return true;
    }
  }

  std::string Type::name() const {
    switch (kind) {
    case TypeKind::I32:
      return "i32";
    case TypeKind::F64:
      return "f64";
    case TypeKind::Bool:
      return "bool";
    case TypeKind::Str:
      return "str";
    case TypeKind::Array:
      return "[" + (element ? element->name() : std::string{"?"}) + "]";
    case TypeKind::Struct:
      return structName.empty() ? std::string{"<struct>"} : structName;
    case TypeKind::TypeParam:
      return typeParamName;
    case TypeKind::Void:
      return "void";
    }

    return "<unknown>";
  }

  std::string llvmType(const Type& type) {
    switch (type.kind) {
    case TypeKind::I32:
      return "i32";
    case TypeKind::F64:
      return "double";
    case TypeKind::Bool:
      return "i1";
    case TypeKind::Str:
    case TypeKind::Array:
      return "ptr";
    case TypeKind::Struct:
      return "%" + type.structName;
    case TypeKind::TypeParam:
      throw CompileError("internal: unsubstituted type parameter");
    case TypeKind::Void:
      return "void";
    }

    return "";
  }

} // namespace noria
