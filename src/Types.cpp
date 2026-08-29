#include "noria/Types.hpp"

#include "noria/Diagnostic.hpp"
#include "noria/SemanticTables.hpp"

#include <type_traits>
#include <utility>

namespace noria {

  Type::ArrayPayload::ArrayPayload(Type elementType)
      : element(std::make_unique<Type>(std::move(elementType))) {}

  Type::ArrayPayload::ArrayPayload(const ArrayPayload& other)
      : element(std::make_unique<Type>(*other.element)) {}

  Type::ArrayPayload& Type::ArrayPayload::operator=(const ArrayPayload& other) {
    if (this != &other) {
      element = std::make_unique<Type>(*other.element);
    }
    return *this;
  }

  Type::Type() = default;
  Type::Type(Storage storage) : storage_(std::move(storage)) {}
  Type::Type(const Type& other) = default;
  Type& Type::operator=(const Type& other) {
    if (this != &other) {
      Type copy(other);
      *this = std::move(copy);
    }
    return *this;
  }
  Type::Type(Type&& other) noexcept = default;
  Type& Type::operator=(Type&& other) noexcept = default;
  Type::~Type() = default;

  Type Type::i32() { return Type(I32Payload{}); }
  Type Type::f64() { return Type(F64Payload{}); }
  Type Type::boolean() { return Type(BoolPayload{}); }
  Type Type::str() { return Type(StrPayload{}); }
  Type Type::voidType() { return Type(VoidPayload{}); }
  Type Type::rawPtr() { return Type(RawPtrPayload{}); }

  Type Type::array(Type elementType) { return Type(ArrayPayload(std::move(elementType))); }

  Type Type::structType(std::string name, std::vector<Type> typeArgs) {
    return Type(StructPayload{std::move(name), std::move(typeArgs)});
  }

  Type Type::typeParam(std::string name) {
    return Type(TypeParameterPayload{std::move(name)});
  }

  Type Type::implementationTag(ImplementationTag tag) {
    return Type(ImplementationTagPayload{tag});
  }

  TypeKind Type::kind() const noexcept {
    return std::visit(
        [](const auto& payload) -> TypeKind {
          using Payload = std::decay_t<decltype(payload)>;
          if constexpr (std::is_same_v<Payload, I32Payload>) {
            return TypeKind::I32;
          } else if constexpr (std::is_same_v<Payload, F64Payload>) {
            return TypeKind::F64;
          } else if constexpr (std::is_same_v<Payload, BoolPayload>) {
            return TypeKind::Bool;
          } else if constexpr (std::is_same_v<Payload, StrPayload>) {
            return TypeKind::Str;
          } else if constexpr (std::is_same_v<Payload, ArrayPayload>) {
            return TypeKind::Array;
          } else if constexpr (std::is_same_v<Payload, StructPayload>) {
            return TypeKind::Struct;
          } else if constexpr (std::is_same_v<Payload, TypeParameterPayload>) {
            return TypeKind::TypeParam;
          } else if constexpr (std::is_same_v<Payload, ImplementationTagPayload>) {
            return TypeKind::ImplTag;
          } else if constexpr (std::is_same_v<Payload, RawPtrPayload>) {
            return TypeKind::RawPtr;
          } else {
            static_assert(std::is_same_v<Payload, VoidPayload>);
            return TypeKind::Void;
          }
        },
        storage_);
  }

  Type& Type::elementType() { return *std::get<ArrayPayload>(storage_).element; }

  const Type& Type::elementType() const { return *std::get<ArrayPayload>(storage_).element; }

  const std::string& Type::structName() const { return std::get<StructPayload>(storage_).name; }

  std::vector<Type>& Type::typeArguments() {
    return std::get<StructPayload>(storage_).typeArguments;
  }

  const std::vector<Type>& Type::typeArguments() const {
    return std::get<StructPayload>(storage_).typeArguments;
  }

  const std::string& Type::typeParameterName() const {
    return std::get<TypeParameterPayload>(storage_).name;
  }

  ImplementationTag Type::implementationTagValue() const {
    return std::get<ImplementationTagPayload>(storage_).tag;
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
    if (kind() != other.kind())
      return false;

    switch (kind()) {
    case TypeKind::Array: return elementType() == other.elementType();
    case TypeKind::Struct:
      return structName() == other.structName() && typeArguments() == other.typeArguments();
    case TypeKind::TypeParam: return typeParameterName() == other.typeParameterName();
    case TypeKind::ImplTag: return implementationTagValue() == other.implementationTagValue();
    case TypeKind::I32:
    case TypeKind::F64:
    case TypeKind::Bool:
    case TypeKind::Str:
    case TypeKind::RawPtr:
    case TypeKind::Void: return true;
    }
    return false;
  }

  std::string Type::name() const {
    if (kind() == TypeKind::Array) {
      return "[" + elementType().name() + "]";
    }

    if (kind() == TypeKind::Struct) {
      if (structName().empty())
        return "<struct>";
      if (typeArguments().empty())
        return structName();
      {
        std::string rendered = structName() + "<";
        for (std::size_t index{}; index < typeArguments().size(); ++index) {
          if (index != 0)
            rendered += ", ";
          rendered += typeArguments()[index].name();
        }
        rendered += ">";
        return rendered;
      }
    }

    if (kind() == TypeKind::TypeParam) {
      return typeParameterName();
    }

    if (kind() == TypeKind::ImplTag) {
      return std::string(implementationTagName(implementationTagValue()));
    }

    if (const TypeKindInfo* info = typeKindInfo(kind()); info && !info->displayName.empty()) {
      return std::string(info->displayName);
    }
    return "<unknown>";
  }

  std::string LLVMType(const Type& type) {
    if (type.kind() == TypeKind::Struct) {
      if (!type.typeArguments().empty()) {
        throw CompileError("internal: unsubstituted generic struct");
      }
      return "%" + type.structName();
    }

    if (type.kind() == TypeKind::TypeParam) {
      throw CompileError("internal: unsubstituted type parameter");
    }

    if (type.kind() == TypeKind::ImplTag) {
      throw CompileError("internal: implementation tag is not a runtime type");
    }

    if (const TypeKindInfo* info = typeKindInfo(type.kind()); info && !info->LLVMName.empty()) {
      return std::string(info->LLVMName);
    }
    return "";
  }

} // namespace noria
