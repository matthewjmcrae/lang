#include "noria/TypeChecker.hpp"

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

  void expectName(const noria::Type& type, const std::string& expected) {
    if (type.name() != expected) {
      std::cerr << "FAIL: expected name '" << expected << "', got '" << type.name() << "'\n";
      ++failures;
    }
  }

} // namespace

int main() {
  using noria::Type;

  expectName(Type::i32(), "i32");
  expectName(Type::f64(), "f64");
  expectName(Type::boolean(), "bool");
  expectName(Type::str(), "str");
  expectName(Type::voidType(), "void");
  expectName(Type::array(Type::i32()), "[i32]");
  expectName(Type::array(Type::boolean()), "[bool]");
  expectName(Type::array(Type::f64()), "[f64]");
  expectName(Type::array(Type::str()), "[str]");
  expectName(Type::structType("Point"), "Point");
  expectName(Type::structType(""), "<struct>");

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
