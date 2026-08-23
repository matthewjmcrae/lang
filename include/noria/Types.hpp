#pragma once

#include <memory>
#include <string>
#include <vector>

namespace noria {

  enum class TypeKind {
    I32,
    F64,
    Bool,
    Str,
    Array,
    Struct,
    TypeParam,
    Void,
  };

  // Kind + payload type representation. Scalars (I32/F64/Bool/Str/Void) only use
  // `kind`; Array carries its element type and Struct carries its name. Later
  // phases add behaviour for the non-scalar kinds without reshaping callers.
  struct Type {
    TypeKind kind = TypeKind::I32;
    std::shared_ptr<Type> element; // Array element type
    std::string structName;        // Struct name
    std::vector<Type> typeArgs;    // Generic struct type arguments
    std::string typeParamName;     // TypeParam name

    Type() = default;
    explicit Type(TypeKind kind) : kind(kind) {}

    static Type i32() { return Type(TypeKind::I32); }
    static Type f64() { return Type(TypeKind::F64); }
    static Type boolean() { return Type(TypeKind::Bool); }
    static Type str() { return Type(TypeKind::Str); }
    static Type voidType() { return Type(TypeKind::Void); }
    static Type array(Type elementType);
    static Type structType(std::string name, std::vector<Type> typeArgs = {});
    static Type typeParam(std::string name);

    bool operator==(const Type& other) const;
    bool operator!=(const Type& other) const { return !(*this == other); }

    // Human-readable name for diagnostics (e.g. "i32", "[bool]", "Point").
    std::string name() const;
  };

  std::string llvmType(const Type& type);

} // namespace noria
