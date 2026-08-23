#pragma once

#include "noria/Types.hpp"

#include <array>
#include <cstddef>
#include <sstream>
#include <string>
#include <string_view>

namespace noria {

  enum class BuiltinId { Print, PrintInt, PrintFloat, PrintChar, Println, Sqrt, Pow, Len };

  enum class MismatchStyle { PerArgument, AllArguments };

  struct BuiltinSignature {
    BuiltinId id;
    std::string_view name;
    std::size_t arity;
    std::array<TypeKind, 2> parameters;
    TypeKind returnKind;
    MismatchStyle style;
  };

  inline constexpr std::array<BuiltinSignature, 8> builtinSignatures{{
      {BuiltinId::Print,
       "print",
       1,
       {TypeKind::Str, TypeKind::Void},
       TypeKind::Void,
       MismatchStyle::PerArgument},
      {BuiltinId::PrintInt,
       "print_int",
       1,
       {TypeKind::I32, TypeKind::Void},
       TypeKind::Void,
       MismatchStyle::PerArgument},
      {BuiltinId::PrintFloat,
       "print_float",
       1,
       {TypeKind::F64, TypeKind::Void},
       TypeKind::Void,
       MismatchStyle::PerArgument},
      {BuiltinId::PrintChar,
       "print_char",
       1,
       {TypeKind::I32, TypeKind::Void},
       TypeKind::Void,
       MismatchStyle::PerArgument},
      {BuiltinId::Println,
       "println",
       0,
       {TypeKind::Void, TypeKind::Void},
       TypeKind::Void,
       MismatchStyle::PerArgument},
      {BuiltinId::Sqrt,
       "sqrt",
       1,
       {TypeKind::F64, TypeKind::Void},
       TypeKind::F64,
       MismatchStyle::PerArgument},
      {BuiltinId::Pow,
       "pow",
       2,
       {TypeKind::F64, TypeKind::F64},
       TypeKind::F64,
       MismatchStyle::AllArguments},
      {BuiltinId::Len,
       "len",
       1,
       {TypeKind::Str, TypeKind::Void},
       TypeKind::I32,
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
