#pragma once

#include "noria/Types.hpp"

#include <array>
#include <cstddef>
#include <sstream>
#include <string>
#include <string_view>

namespace noria {

  enum class BuiltinId {
    Print,
    PrintInt,
    PrintFloat,
    PrintChar,
    Println,
    Sqrt,
    Pow,
    Len,
    RtAlloc,
    RtRealloc,
    RtRelease,
    RtSizeof,
    RtLoad,
    RtStore,
    RtLoadPtr,
    RtStorePtr,
    RtLoadI32,
    RtStoreI32,
    RtTrap,
    RtNull,
    RtPtrEq,
  };

  enum class Visibility { Public, Internal };

  enum class MismatchStyle { PerArgument, AllArguments };

  struct BuiltinSignature {
    BuiltinId id;
    std::string_view name;
    Visibility visibility;
    std::size_t arity;
    std::array<TypeKind, 3> parameters;
    TypeKind returnKind;
    MismatchStyle style;
  };

  inline constexpr std::array<BuiltinSignature, 21> builtinSignatures{{
      {BuiltinId::Print,
       "print",
       Visibility::Public,
       1,
       {TypeKind::Str, TypeKind::Void, TypeKind::Void},
       TypeKind::Void,
       MismatchStyle::PerArgument},
      {BuiltinId::PrintInt,
       "print_int",
       Visibility::Public,
       1,
       {TypeKind::I32, TypeKind::Void, TypeKind::Void},
       TypeKind::Void,
       MismatchStyle::PerArgument},
      {BuiltinId::PrintFloat,
       "print_float",
       Visibility::Public,
       1,
       {TypeKind::F64, TypeKind::Void, TypeKind::Void},
       TypeKind::Void,
       MismatchStyle::PerArgument},
      {BuiltinId::PrintChar,
       "print_char",
       Visibility::Public,
       1,
       {TypeKind::I32, TypeKind::Void, TypeKind::Void},
       TypeKind::Void,
       MismatchStyle::PerArgument},
      {BuiltinId::Println,
       "println",
       Visibility::Public,
       0,
       {TypeKind::Void, TypeKind::Void, TypeKind::Void},
       TypeKind::Void,
       MismatchStyle::PerArgument},
      {BuiltinId::Sqrt,
       "sqrt",
       Visibility::Public,
       1,
       {TypeKind::F64, TypeKind::Void, TypeKind::Void},
       TypeKind::F64,
       MismatchStyle::PerArgument},
      {BuiltinId::Pow,
       "pow",
       Visibility::Public,
       2,
       {TypeKind::F64, TypeKind::F64, TypeKind::Void},
       TypeKind::F64,
       MismatchStyle::AllArguments},
      {BuiltinId::Len,
       "len",
       Visibility::Public,
       1,
       {TypeKind::Str, TypeKind::Void, TypeKind::Void},
       TypeKind::I32,
       MismatchStyle::PerArgument},
      {BuiltinId::RtAlloc,
       "__rt_alloc",
       Visibility::Internal,
       1,
       {TypeKind::I32, TypeKind::Void, TypeKind::Void},
       TypeKind::RawPtr,
       MismatchStyle::PerArgument},
      {BuiltinId::RtRealloc,
       "__rt_realloc",
       Visibility::Internal,
       2,
       {TypeKind::RawPtr, TypeKind::I32, TypeKind::Void},
       TypeKind::RawPtr,
       MismatchStyle::PerArgument},
      {BuiltinId::RtRelease,
       "__rt_release",
       Visibility::Internal,
       1,
       {TypeKind::RawPtr, TypeKind::Void, TypeKind::Void},
       TypeKind::Void,
       MismatchStyle::PerArgument},
      {BuiltinId::RtSizeof,
       "__rt_sizeof",
       Visibility::Internal,
       0,
       {TypeKind::Void, TypeKind::Void, TypeKind::Void},
       TypeKind::I32,
       MismatchStyle::PerArgument},
      {BuiltinId::RtLoad,
       "__rt_load",
       Visibility::Internal,
       2,
       {TypeKind::RawPtr, TypeKind::I32, TypeKind::Void},
       TypeKind::TypeParam,
       MismatchStyle::PerArgument},
      {BuiltinId::RtStore,
       "__rt_store",
       Visibility::Internal,
       3,
       {TypeKind::RawPtr, TypeKind::I32, TypeKind::TypeParam},
       TypeKind::Void,
       MismatchStyle::PerArgument},
      {BuiltinId::RtLoadPtr,
       "__rt_load_ptr",
       Visibility::Internal,
       2,
       {TypeKind::RawPtr, TypeKind::I32, TypeKind::Void},
       TypeKind::RawPtr,
       MismatchStyle::PerArgument},
      {BuiltinId::RtStorePtr,
       "__rt_store_ptr",
       Visibility::Internal,
       3,
       {TypeKind::RawPtr, TypeKind::I32, TypeKind::RawPtr},
       TypeKind::Void,
       MismatchStyle::PerArgument},
      {BuiltinId::RtLoadI32,
       "__rt_load_i32",
       Visibility::Internal,
       2,
       {TypeKind::RawPtr, TypeKind::I32, TypeKind::Void},
       TypeKind::I32,
       MismatchStyle::PerArgument},
      {BuiltinId::RtStoreI32,
       "__rt_store_i32",
       Visibility::Internal,
       3,
       {TypeKind::RawPtr, TypeKind::I32, TypeKind::I32},
       TypeKind::Void,
       MismatchStyle::PerArgument},
      {BuiltinId::RtTrap,
       "__rt_trap",
       Visibility::Internal,
       1,
       {TypeKind::Str, TypeKind::Void, TypeKind::Void},
       TypeKind::Void,
       MismatchStyle::PerArgument},
      {BuiltinId::RtNull,
       "__rt_null",
       Visibility::Internal,
       0,
       {TypeKind::Void, TypeKind::Void, TypeKind::Void},
       TypeKind::RawPtr,
       MismatchStyle::PerArgument},
      {BuiltinId::RtPtrEq,
       "__rt_ptr_eq",
       Visibility::Internal,
       2,
       {TypeKind::RawPtr, TypeKind::RawPtr, TypeKind::Void},
       TypeKind::Bool,
       MismatchStyle::PerArgument},
  }};

  inline const BuiltinSignature* lookupBuiltin(std::string_view name) {
    for (const BuiltinSignature& signature : builtinSignatures) {
      if (signature.name == name)
        return &signature;
    }
    return nullptr;
  }

  inline bool builtinArityMatches(const BuiltinSignature& signature, std::size_t argumentCount) {
    return argumentCount == signature.arity;
  }

  inline std::string formatBuiltinArityError(const BuiltinSignature& signature) {
    std::ostringstream out;
    out << signature.name << " expects " << signature.arity;
    if (signature.arity == 1)
      out << " argument";
    else
      out << " arguments";
    return out.str();
  }

  inline std::string formatBuiltinPerArgumentMismatch(std::string_view name, TypeKind expected,
                                                      std::string_view actualName) {
    return std::string(name) + " expects " + Type(expected).name() + ", got " +
           std::string(actualName);
  }

  inline std::string formatBuiltinAllArgumentsMismatch(std::string_view name, TypeKind expected,
                                                       std::string_view firstName,
                                                       std::string_view secondName) {
    return std::string(name) + " expects " + Type(expected).name() + " arguments, got " +
           std::string(firstName) + " and " + std::string(secondName);
  }

} // namespace noria
