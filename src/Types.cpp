#include "noria/Types.hpp"

#include "noria/Diagnostic.hpp"
#include "noria/SemanticTables.hpp"

#include <unordered_map>
#include <utility>

namespace noria {

  Type Type::array(Type elementType) {
    Type type(TypeKind::Array);
    type.element = std::make_shared<Type>(std::move(elementType));
    return type;
  }

  Type Type::structType(std::string name, std::vector<Type> typeArgs) {
    Type type(TypeKind::Struct);
    type.structName = std::move(name);
    type.typeArgs = std::move(typeArgs);
    return type;
  }

  Type Type::typeParam(std::string name) {
    Type type(TypeKind::TypeParam);
    type.typeParamName = std::move(name);
    return type;
  }

  Type Type::implementationTag(ImplementationTag tag) {
    Type type(TypeKind::ImplTag);
    type.implTag = tag;
    return type;
  }

  std::optional<ImplementationTag> implementationTagFromName(std::string_view name) {
    const auto tag = implementationTagNameTable().find(name);
    if (tag != implementationTagNameTable().end()) {
      return tag->second;
    }
    return std::nullopt;
  }

  std::string_view implementationTagName(ImplementationTag tag) {
    if (const ImplementationTagInfo* info = implementationTagInfo(tag)) {
      return info->name;
    }
    return "";
  }

  bool Type::operator==(const Type& other) const {
    if (kind != other.kind)
      return false;

    using EqualityCheck = bool (*)(const Type&, const Type&);
    static const std::unordered_map<TypeKind, EqualityCheck, EnumHash<TypeKind>> equalityChecks = {
        {TypeKind::Array,
         [](const Type& left, const Type& right) {
           if (!left.element || !right.element)
             return left.element == right.element;
           return *left.element == *right.element;
         }},
        {TypeKind::Struct,
         [](const Type& left, const Type& right) {
           return left.structName == right.structName && left.typeArgs == right.typeArgs;
         }},
        {TypeKind::TypeParam,
         [](const Type& left, const Type& right) {
           return left.typeParamName == right.typeParamName;
         }},
        {TypeKind::ImplTag,
         [](const Type& left, const Type& right) { return left.implTag == right.implTag; }},
    };

    const auto check = equalityChecks.find(kind);
    if (check != equalityChecks.end()) {
      return check->second(*this, other);
    }

    return true;
  }

  std::string Type::name() const {
    if (kind == TypeKind::Array) {
      return "[" + (element ? element->name() : std::string{"?"}) + "]";
    }

    if (kind == TypeKind::Struct) {
      if (structName.empty())
        return "<struct>";
      if (typeArgs.empty())
        return structName;
      {
        std::string rendered = structName + "<";
        for (std::size_t index{}; index < typeArgs.size(); ++index) {
          if (index != 0)
            rendered += ", ";
          rendered += typeArgs[index].name();
        }
        rendered += ">";
        return rendered;
      }
    }

    if (kind == TypeKind::TypeParam) {
      return typeParamName;
    }

    if (kind == TypeKind::ImplTag) {
      return std::string(implementationTagName(implTag));
    }

    if (const TypeKindInfo* info = typeKindInfo(kind); info && !info->displayName.empty()) {
      return std::string(info->displayName);
    }
    return "<unknown>";
  }

  std::string LLVMType(const Type& type) {
    if (type.kind == TypeKind::Struct) {
      if (!type.typeArgs.empty()) {
        throw CompileError("internal: unsubstituted generic struct");
      }
      return "%" + type.structName;
    }

    if (type.kind == TypeKind::TypeParam) {
      throw CompileError("internal: unsubstituted type parameter");
    }

    if (type.kind == TypeKind::ImplTag) {
      throw CompileError("internal: implementation tag is not a runtime type");
    }

    if (const TypeKindInfo* info = typeKindInfo(type.kind); info && !info->LLVMName.empty()) {
      return std::string(info->LLVMName);
    }
    return "";
  }

} // namespace noria
