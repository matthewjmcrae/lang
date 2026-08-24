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
  expect(printDescriptor->visibility == noria::Visibility::Public, "print visibility");
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

  const BuiltinSignature* lenDescriptor = lookupBuiltin("len");
  expect(lenDescriptor != nullptr, "lookup len");
  expect(lenDescriptor->id == BuiltinId::Len, "len id");
  expect(lenDescriptor->arity == 1, "len arity");
  expect(lenDescriptor->parameters[0] == TypeKind::Str, "len parameter kind");
  expect(lenDescriptor->returnKind == TypeKind::I32, "len return kind");
  expect(lenDescriptor->style == MismatchStyle::PerArgument, "len mismatch style");

  const BuiltinSignature* rtAllocDescriptor = lookupBuiltin("__rt_alloc");
  expect(rtAllocDescriptor != nullptr, "lookup __rt_alloc");
  expect(rtAllocDescriptor->id == BuiltinId::RtAlloc, "__rt_alloc id");
  expect(rtAllocDescriptor->visibility == noria::Visibility::Internal, "__rt_alloc visibility");
  expect(rtAllocDescriptor->parameters[0] == TypeKind::I32, "__rt_alloc parameter kind");
  expect(rtAllocDescriptor->returnKind == TypeKind::RawPtr, "__rt_alloc return kind");

  const BuiltinSignature* rtReallocDescriptor = lookupBuiltin("__rt_realloc");
  expect(rtReallocDescriptor != nullptr, "lookup __rt_realloc");
  expect(rtReallocDescriptor->parameters[0] == TypeKind::RawPtr, "__rt_realloc pointer kind");
  expect(rtReallocDescriptor->parameters[1] == TypeKind::I32, "__rt_realloc size kind");

  const BuiltinSignature* rtReleaseDescriptor = lookupBuiltin("__rt_release");
  expect(rtReleaseDescriptor != nullptr, "lookup __rt_release");
  expect(rtReleaseDescriptor->returnKind == TypeKind::Void, "__rt_release return kind");

  const BuiltinSignature* rtSizeofDescriptor = lookupBuiltin("__rt_sizeof");
  expect(rtSizeofDescriptor != nullptr, "lookup __rt_sizeof");
  expect(rtSizeofDescriptor->id == BuiltinId::RtSizeof, "__rt_sizeof id");
  expect(rtSizeofDescriptor->arity == 0, "__rt_sizeof arity");
  expect(rtSizeofDescriptor->returnKind == TypeKind::I32, "__rt_sizeof return kind");

  const BuiltinSignature* rtLoadDescriptor = lookupBuiltin("__rt_load");
  expect(rtLoadDescriptor != nullptr, "lookup __rt_load");
  expect(rtLoadDescriptor->id == BuiltinId::RtLoad, "__rt_load id");
  expect(rtLoadDescriptor->parameters[0] == TypeKind::RawPtr, "__rt_load pointer kind");
  expect(rtLoadDescriptor->parameters[1] == TypeKind::I32, "__rt_load index kind");
  expect(rtLoadDescriptor->returnKind == TypeKind::TypeParam, "__rt_load witness return kind");

  const BuiltinSignature* rtStoreDescriptor = lookupBuiltin("__rt_store");
  expect(rtStoreDescriptor != nullptr, "lookup __rt_store");
  expect(rtStoreDescriptor->id == BuiltinId::RtStore, "__rt_store id");
  expect(rtStoreDescriptor->parameters[2] == TypeKind::TypeParam, "__rt_store witness value kind");

  const BuiltinSignature* rtTrapDescriptor = lookupBuiltin("__rt_trap");
  expect(rtTrapDescriptor != nullptr, "lookup __rt_trap");
  expect(rtTrapDescriptor->id == BuiltinId::RtTrap, "__rt_trap id");
  expect(rtTrapDescriptor->visibility == noria::Visibility::Internal, "__rt_trap visibility");
  expect(rtTrapDescriptor->parameters[0] == TypeKind::Str, "__rt_trap parameter kind");
  expect(rtTrapDescriptor->returnKind == TypeKind::Void, "__rt_trap return kind");

  const BuiltinSignature* rtNullDescriptor = lookupBuiltin("__rt_null");
  expect(rtNullDescriptor != nullptr, "lookup __rt_null");
  expect(rtNullDescriptor->id == BuiltinId::RtNull, "__rt_null id");
  expect(rtNullDescriptor->arity == 0, "__rt_null arity");
  expect(rtNullDescriptor->returnKind == TypeKind::RawPtr, "__rt_null return kind");

  const BuiltinSignature* rtPtrEqDescriptor = lookupBuiltin("__rt_ptr_eq");
  expect(rtPtrEqDescriptor != nullptr, "lookup __rt_ptr_eq");
  expect(rtPtrEqDescriptor->id == BuiltinId::RtPtrEq, "__rt_ptr_eq id");
  expect(rtPtrEqDescriptor->arity == 2, "__rt_ptr_eq arity");
  expect(rtPtrEqDescriptor->parameters[0] == TypeKind::RawPtr, "__rt_ptr_eq left kind");
  expect(rtPtrEqDescriptor->parameters[1] == TypeKind::RawPtr, "__rt_ptr_eq right kind");
  expect(rtPtrEqDescriptor->returnKind == TypeKind::Bool, "__rt_ptr_eq return kind");

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
  expect(Type(lenDescriptor->parameters[0]) == Type::str(), "len parameter type");
  expect(Type(lenDescriptor->returnKind) == Type::i32(), "len return type");
  expect(Type::rawPtr().name() == "__rt_ptr", "raw ptr name");
  expect(noria::llvmType(Type::rawPtr()) == "ptr", "raw ptr llvm type");
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
