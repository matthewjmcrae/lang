#include "noria/Types.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace {

  int failures = 0;

  void expect(bool condition, const char* message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      ++failures;
    }
  }

  void expectName(const noria::Type& type, const std::string& expected,
                  const char* message = "type name") {
    if (type.name() != expected) {
      std::cerr << "FAIL: " << message << ": expected name '" << expected << "', got '"
                << type.name() << "'\n";
      ++failures;
    }
  }

  void assign(noria::Type& destination, const noria::Type& source) { destination = source; }

  void expectLlvm(const noria::Type& type, const std::string& expected) {
    if (noria::LLVMType(type) != expected) {
      std::cerr << "FAIL: expected llvm type '" << expected << "', got '" << noria::LLVMType(type)
                << "'\n";
      ++failures;
    }
  }

} // namespace

int main() {
  using noria::Type;
  using noria::TypeKind;

  static_assert(std::is_copy_constructible_v<Type>);
  static_assert(std::is_copy_assignable_v<Type>);
  static_assert(std::is_nothrow_move_constructible_v<Type>);
  static_assert(!std::is_constructible_v<Type, TypeKind>);

  expect(Type().kind() == TypeKind::I32, "default type is i32");
  expect(Type::i32().kind() == TypeKind::I32, "i32 kind");
  expect(Type::f64().kind() == TypeKind::F64, "f64 kind");
  expect(Type::boolean().kind() == TypeKind::Bool, "bool kind");
  expect(Type::str().kind() == TypeKind::Str, "str kind");
  expect(Type::voidType().kind() == TypeKind::Void, "void kind");
  expect(Type::rawPtr().kind() == TypeKind::RawPtr, "raw pointer kind");
  expectName(Type::i32(), "i32");
  expectName(Type::f64(), "f64");
  expectName(Type::boolean(), "bool");
  expectName(Type::str(), "str");
  expectName(Type::voidType(), "void");
  expectName(Type::rawPtr(), "__rt_ptr");
  expectName(Type::array(Type::i32()), "[i32]");
  expectName(Type::array(Type::boolean()), "[bool]");
  expectName(Type::array(Type::f64()), "[f64]");
  expectName(Type::array(Type::str()), "[str]");
  expectName(Type::structType("Point"), "Point");
  expectName(Type::structType(""), "<struct>");

  expectLlvm(Type::i32(), "i32");
  expectLlvm(Type::f64(), "double");
  expectLlvm(Type::boolean(), "i1");
  expectLlvm(Type::voidType(), "void");
  expectLlvm(Type::str(), "ptr");
  expectLlvm(Type::rawPtr(), "ptr");
  expectLlvm(Type::array(Type::i32()), "ptr");
  expectLlvm(Type::structType("Point"), "%Point");

  expect(Type::i32() == Type::i32(), "i32 equals i32");
  expect(Type::f64() == Type::f64(), "f64 equals f64");
  expect(Type::str() == Type::str(), "str equals str");
  expect(Type::i32() != Type::f64(), "i32 not equal to f64");
  expect(Type::boolean() != Type::str(), "bool not equal to str");

  const Type arrayOfI32 = Type::array(Type::i32());
  expect(arrayOfI32 == Type::array(Type::i32()), "array[i32] equals array[i32]");
  expect(arrayOfI32 != Type::array(Type::boolean()), "array[i32] not equal to array[bool]");
  expect(arrayOfI32 != Type::i32(), "array[i32] not equal to i32");

  const Type nestedArray = Type::array(Type::array(Type::i32()));
  expectName(nestedArray, "[[i32]]");
  expect(nestedArray == Type::array(Type::array(Type::i32())), "nested array equality");

  Type copiedArray = nestedArray;
  copiedArray.elementType().elementType() = Type::f64();
  expectName(nestedArray, "[[i32]]", "copying an array deep-copies nested elements");
  expectName(copiedArray, "[[f64]]", "copied nested array can change independently");

  Type assignedArray = Type::array(Type::boolean());
  assignedArray = nestedArray;
  assignedArray.elementType().elementType() = Type::str();
  expectName(nestedArray, "[[i32]]", "array assignment deep-copies nested elements");
  expectName(assignedArray, "[[str]]", "assigned array can change independently");
  assign(assignedArray, assignedArray);
  expectName(assignedArray, "[[str]]", "array self-assignment preserves the value");

  const Type genericBox = Type::structType("Box", {Type::array(Type::i32())});
  Type copiedBox = genericBox;
  copiedBox.typeArguments()[0].elementType() = Type::f64();
  expectName(genericBox, "Box<[i32]>", "copying struct type arguments deep-copies arrays");
  expectName(copiedBox, "Box<[f64]>", "copied struct type arguments can change independently");
  expect(genericBox.structName() == "Box", "struct name accessor");
  expect(genericBox.typeArguments().size() == 1, "struct type argument accessor");

  Type movedArray = std::move(copiedArray);
  expectName(movedArray, "[[f64]]", "moving an array preserves its value");

  const Type typeParameter = Type::typeParam("T");
  expect(typeParameter.kind() == TypeKind::TypeParam, "type parameter kind");
  expect(typeParameter.typeParameterName() == "T", "type parameter name accessor");
  expectName(typeParameter, "T");

  const Type implementationTag = Type::implementationTag(noria::ImplementationTag::List);
  expect(implementationTag.kind() == TypeKind::ImplTag, "implementation tag kind");
  expect(implementationTag.implementationTagValue() == noria::ImplementationTag::List,
         "implementation tag accessor");
  expectName(implementationTag, "list");

  bool wrongAccessorRejected = false;
  try {
    (void)Type::i32().elementType();
  } catch (const std::bad_variant_access&) {
    wrongAccessorRejected = true;
  }
  expect(wrongAccessorRejected, "wrong payload accessor throws bad_variant_access");

  const Type point = Type::structType("Point");
  expect(point == Type::structType("Point"), "struct Point equals Point");
  expect(point != Type::structType("Line"), "struct Point not equal to Line");
  expect(point != Type::i32(), "struct Point not equal to i32");

  if (failures != 0) {
    std::cerr << failures << " type representation test(s) failed\n";
    return EXIT_FAILURE;
  }

  std::cout << "type representation tests ok\n";
  return EXIT_SUCCESS;
}
