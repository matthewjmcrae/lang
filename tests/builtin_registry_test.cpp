#include "noria/Builtins.hpp"
#include "noria/Types.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

  int failures = 0;

  void expect(bool condition, const char* message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      ++failures;
    }
  }

} // namespace

int main() {
  using noria::BuiltinId;
  using noria::BuiltinSignature;
  using noria::MismatchStyle;
  using noria::Type;
  using noria::TypeKind;
  using noria::builtinArityMatches;
  using noria::builtinSignatures;
  using noria::formatBuiltinAllArgumentsMismatch;
  using noria::formatBuiltinArityError;
  using noria::formatBuiltinPerArgumentMismatch;
  using noria::lookupBuiltin;

  const BuiltinSignature* printDescriptor = lookupBuiltin("print");
  expect(printDescriptor != nullptr, "lookup print");
  expect(printDescriptor->id == BuiltinId::Print, "print id");
  expect(printDescriptor->name == "print", "print name");
  expect(printDescriptor->arity == 1, "print arity");
  expect(printDescriptor->returnKind == TypeKind::Void, "print return kind");

  const BuiltinSignature* powDescriptor = lookupBuiltin("pow");
  expect(powDescriptor != nullptr, "lookup pow");
  expect(powDescriptor->id == BuiltinId::Pow, "pow id");
  expect(powDescriptor->arity == 2, "pow arity");
  expect(powDescriptor->returnKind == TypeKind::F64, "pow return kind");
  expect(powDescriptor->style == MismatchStyle::AllArguments, "pow mismatch style");

  const BuiltinSignature* printlnDescriptor = lookupBuiltin("println");
  expect(printlnDescriptor != nullptr, "lookup println");

  const BuiltinSignature* printIntDescriptor = lookupBuiltin("print_int");
  expect(printIntDescriptor != nullptr, "lookup print_int");

  expect(lookupBuiltin("read_char") == nullptr, "read_char is not a builtin");
  expect(lookupBuiltin("") == nullptr, "empty name is not a builtin");

  for (std::size_t index{}; index < builtinSignatures.size(); ++index) {
    const BuiltinSignature& signature = builtinSignatures[index];
    expect(static_cast<std::size_t>(signature.id) == index, "enum index matches table order");
    expect(lookupBuiltin(signature.name) == &signature, "lookup returns table entry");
  }

  expect(builtinArityMatches(*printlnDescriptor, 0), "println accepts zero arguments");
  expect(!builtinArityMatches(*printlnDescriptor, 1), "println rejects one argument");
  expect(builtinArityMatches(*printIntDescriptor, 1), "print_int accepts one argument");
  expect(!builtinArityMatches(*printIntDescriptor, 0), "print_int rejects zero arguments");
  expect(builtinArityMatches(*powDescriptor, 2), "pow accepts two arguments");
  expect(!builtinArityMatches(*powDescriptor, 1), "pow rejects one argument");

  expect(formatBuiltinArityError(*printlnDescriptor) == "println expects 0 arguments",
         "println zero-argument arity message");
  expect(formatBuiltinArityError(*printIntDescriptor) == "print_int expects 1 argument",
         "single-argument arity message");
  expect(formatBuiltinArityError(*powDescriptor) == "pow expects 2 arguments",
         "two-argument arity message");

  expect(formatBuiltinPerArgumentMismatch("print_int", TypeKind::I32, "str") ==
             "print_int expects i32, got str",
         "per-argument mismatch message");
  expect(formatBuiltinAllArgumentsMismatch("pow", TypeKind::F64, "i32", "str") ==
             "pow expects f64 arguments, got i32 and str",
         "pow combined mismatch message");

  expect(Type(printDescriptor->parameters[0]) == Type::str(), "print parameter type");
  expect(Type(powDescriptor->parameters[0]) == Type::f64(), "pow first parameter type");
  expect(Type(powDescriptor->parameters[1]) == Type::f64(), "pow second parameter type");
  expect(Type(powDescriptor->returnKind) == Type::f64(), "pow return type");

  if (failures != 0) {
    std::cerr << failures << " builtin registry test(s) failed\n";
    return EXIT_FAILURE;
  }

  std::cout << "builtin registry tests ok\n";
  return EXIT_SUCCESS;
}
