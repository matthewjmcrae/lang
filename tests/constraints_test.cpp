#include "noria/Constraints.hpp"
#include "noria/Types.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

  int failures = 0;

  void expect(bool condition, const char* message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      ++failures;
    }
  }

  void expectOperations(noria::ImplementationTag tag,
                        const std::vector<noria::RequiredOperation>& expected) {
    const std::vector<noria::RequiredOperation> actual = noria::requiredOperations(tag);
    if (actual != expected) {
      std::cerr << "FAIL: requiredOperations for tag " << static_cast<int>(tag)
                << " size mismatch\n";
      ++failures;
    }
  }

} // namespace

int main() {
  using noria::ImplementationTag;
  using noria::RequiredOperation;
  using noria::Type;

  expectOperations(ImplementationTag::Arr, {});
  expectOperations(ImplementationTag::List, {});
  expectOperations(ImplementationTag::Bst,
                   {RequiredOperation::LessThan, RequiredOperation::Equality});
  expectOperations(ImplementationTag::Hashmap,
                   {RequiredOperation::Equality, RequiredOperation::Hash});

  expect(noria::supportsOperation(Type::i32(), RequiredOperation::LessThan), "i32 supports <");
  expect(noria::supportsOperation(Type::f64(), RequiredOperation::Equality), "f64 supports ==");
  expect(!noria::supportsOperation(Type::str(), RequiredOperation::LessThan), "str lacks <");
  expect(noria::supportsOperation(Type::str(), RequiredOperation::Hash), "str supports hash");
  expect(!noria::supportsOperation(Type::f64(), RequiredOperation::Hash), "f64 lacks hash");
  expect(!noria::supportsOperation(Type::array(Type::i32()), RequiredOperation::Equality),
         "array lacks ==");

  expect(std::string(noria::operationName(RequiredOperation::LessThan)) == "<", "name <");
  expect(std::string(noria::operationName(RequiredOperation::Equality)) == "==", "name ==");
  expect(std::string(noria::operationName(RequiredOperation::Hash)) == "hash", "name hash");

  if (failures != 0) {
    std::cerr << failures << " constraint test(s) failed\n";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
