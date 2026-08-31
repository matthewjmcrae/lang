#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace noria {

  enum class ImplementationTag {
    Arr,
    List,
    Bst,
    Hashmap, //hashset lexes to hashmap
  };

  enum class StandardContainer {
    Sequence,
    Dictionary,
    Set,
  };

  enum class TypeKind {
    I32,
    F64,
    Bool,
    Str,
    Array,
    Struct,
    // A named placeholder declared by a generic function or struct, such as `T` in `Box<T>`.
    TypeParam,
    ImplTag,
    RawPtr,
    Void,
  };

  // A type is represented by exactly one variant alternative. Array elements are owned by
  // their enclosing type and copied recursively, so copied type trees can be normalized
  // independently.
  class Type {
  public:
    Type();
    Type(const Type& other);
    Type& operator=(const Type& other);
    Type(Type&& other) noexcept;
    Type& operator=(Type&& other) noexcept;
    ~Type();

    static Type i32();
    static Type f64();
    static Type boolean();
    static Type str();
    static Type voidType();
    static Type rawPtr();
    static Type array(Type elementType);
    static Type structType(std::string name, std::vector<Type> typeArgs = {});
    static Type typeParam(std::string name);
    static Type implementationTag(ImplementationTag tag);

    TypeKind kind() const noexcept;
    Type& elementType();
    const Type& elementType() const;
    const std::string& structName() const;
    std::vector<Type>& typeArguments();
    const std::vector<Type>& typeArguments() const;
    const std::string& typeParameterName() const;
    ImplementationTag implementationTagValue() const;

    bool operator==(const Type& other) const;
    bool operator!=(const Type& other) const { return !(*this == other); }

    // Human-readable name for diagnostics (e.g. "i32", "[bool]", "Point").
    std::string name() const;

  private:
    struct I32Payload {};
    struct F64Payload {};
    struct BoolPayload {};
    struct StrPayload {};
    struct ArrayPayload {
      explicit ArrayPayload(Type elementType);
      ArrayPayload(const ArrayPayload& other);
      ArrayPayload& operator=(const ArrayPayload& other);
      ArrayPayload(ArrayPayload&&) noexcept = default;
      ArrayPayload& operator=(ArrayPayload&&) noexcept = default;

      std::unique_ptr<Type> element;
    };
    struct StructPayload {
      std::string name;
      std::vector<Type> typeArguments;
    };
    struct TypeParameterPayload {
      std::string name;
    };
    struct ImplementationTagPayload {
      ImplementationTag tag;
    };
    struct RawPtrPayload {};
    struct VoidPayload {};

    using Storage = std::variant<I32Payload, F64Payload, BoolPayload, StrPayload, ArrayPayload,
                                 StructPayload, TypeParameterPayload, ImplementationTagPayload,
                                 RawPtrPayload, VoidPayload>;

    explicit Type(Storage storage);

    Storage storage_;
  };

  std::optional<ImplementationTag> implementationTagFromName(std::string_view name);
  std::string_view implementationTagName(ImplementationTag tag);

  std::string LLVMType(const Type& type);

} // namespace noria
