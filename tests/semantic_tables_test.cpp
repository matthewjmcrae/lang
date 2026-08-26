#include "noria/SemanticTables.hpp"

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

  void expectText(std::string_view actual, std::string_view expected, const char* message) {
    if (actual != expected) {
      std::cerr << "FAIL: " << message << ": expected '" << expected << "', got '" << actual
                << "'\n";
      ++failures;
    }
  }

} // namespace

int main() {
  using noria::ImplementationTag;
  using noria::RequiredOperation;
  using noria::TypeKind;
  using noria::ast::BinaryOperator;
  using noria::ast::UnaryOperator;

  expect(noria::binaryOperatorTable().size() == 18, "all binary operators have metadata");
  expect(noria::unaryOperatorTable().size() == 3, "all unary operators have metadata");
  expect(noria::typeKindTable().size() == 10, "all type kinds have metadata");
  expect(noria::implementationTagTable().size() == 4, "all implementation tags have metadata");
  expect(noria::requiredOperationTable().size() == 3, "all required operations have metadata");

  const std::optional<ImplementationTag> hashmapTag =
      noria::implementationTagFromName("hashmap");
  const std::optional<ImplementationTag> hashsetTag =
      noria::implementationTagFromName("hashset");
  expect(hashmapTag && *hashmapTag == ImplementationTag::Hashmap,
         "hashmap spelling resolves to hashmap tag");
  expect(hashsetTag && *hashsetTag == ImplementationTag::Hashmap,
         "hashset spelling resolves to hashmap tag");
  expect(hashmapTag == hashsetTag, "hashmap and hashset spellings resolve identically");

  const noria::BinaryOperatorInfo* addInfo = noria::binaryOperatorInfo(BinaryOperator::Add);
  expect(addInfo != nullptr, "add operator info exists");
  if (addInfo != nullptr) {
    expectText(addInfo->symbol, "+", "add symbol");
    expect(addInfo->typeCheckRule == noria::BinaryTypeCheckRule::Numeric, "add type rule");
    expectText(addInfo->LLVMIntegerInstruction, "add", "add integer LLVM instruction");
    expectText(addInfo->LLVMFloatInstruction, "fadd", "add float LLVM instruction");
    expect(addInfo->integerSafetyRule == noria::IntegerSafetyRule::None,
           "add has no integer safety rule");
  }

  const noria::BinaryOperatorInfo* divideInfo = noria::binaryOperatorInfo(BinaryOperator::Divide);
  expect(divideInfo != nullptr, "divide operator info exists");
  if (divideInfo != nullptr) {
    expect(divideInfo->integerSafetyRule == noria::IntegerSafetyRule::SignedDivisionOrRemainder,
           "divide checks signed division safety");
  }

  const noria::BinaryOperatorInfo* moduloInfo = noria::binaryOperatorInfo(BinaryOperator::Modulo);
  expect(moduloInfo != nullptr, "modulo operator info exists");
  if (moduloInfo != nullptr) {
    expect(moduloInfo->integerSafetyRule == noria::IntegerSafetyRule::SignedDivisionOrRemainder,
           "modulo checks signed remainder safety");
  }

  const noria::BinaryOperatorInfo* shlInfo = noria::binaryOperatorInfo(BinaryOperator::Shl);
  const noria::BinaryOperatorInfo* shrInfo = noria::binaryOperatorInfo(BinaryOperator::Shr);
  expect(shlInfo != nullptr && shlInfo->integerSafetyRule == noria::IntegerSafetyRule::ShiftCount,
         "left shift checks count range");
  expect(shrInfo != nullptr && shrInfo->integerSafetyRule == noria::IntegerSafetyRule::ShiftCount,
         "right shift checks count range");

  const noria::BinaryOperatorInfo* equalInfo = noria::binaryOperatorInfo(BinaryOperator::Equal);
  expect(equalInfo != nullptr, "equality operator info exists");
  if (equalInfo != nullptr) {
    expect(equalInfo->comparison, "equality is comparison");
    expectText(equalInfo->LLVMIntegerPredicate, "eq", "equality integer predicate");
    expectText(equalInfo->LLVMFloatPredicate, "oeq", "equality float predicate");
  }

  const noria::BinaryOperatorInfo* notEqualInfo =
      noria::binaryOperatorInfo(BinaryOperator::NotEqual);
  expect(notEqualInfo != nullptr, "inequality operator info exists");
  if (notEqualInfo != nullptr) {
    expect(notEqualInfo->comparison, "inequality is comparison");
    expectText(notEqualInfo->LLVMIntegerPredicate, "ne", "inequality integer predicate");
    expectText(notEqualInfo->LLVMFloatPredicate, "une", "inequality float predicate is unordered");
  }

  const noria::BinaryOperatorInfo* andInfo = noria::binaryOperatorInfo(BinaryOperator::And);
  expect(andInfo != nullptr && andInfo->shortCircuit, "logical and short-circuits");

  expect(noria::binaryOperatorFromSymbol(">>") == BinaryOperator::Shr, "shift symbol lookup");
  expect(!noria::binaryOperatorFromSymbol("@"), "unknown binary symbol is absent");

  const noria::UnaryOperatorInfo* notInfo = noria::unaryOperatorInfo(UnaryOperator::Not);
  expect(notInfo != nullptr, "not operator info exists");
  if (notInfo != nullptr) {
    expectText(notInfo->symbol, "!", "not symbol");
    expect(notInfo->typeCheckRule == noria::UnaryTypeCheckRule::Boolean, "not type rule");
    expect(notInfo->codegenRule == noria::UnaryCodegenRule::LogicalNot, "not codegen rule");
  }

  expect(noria::unaryOperatorFromSymbol("~") == UnaryOperator::BitNot, "bit-not symbol lookup");

  const noria::TypeKindInfo* i32Info = noria::typeKindInfo(TypeKind::I32);
  expect(i32Info != nullptr, "i32 type info exists");
  if (i32Info != nullptr) {
    expectText(i32Info->displayName, "i32", "i32 display name");
    expectText(i32Info->LLVMName, "i32", "i32 LLVM name");
    expectText(i32Info->mangleAtom, "s.i32", "i32 mangle atom");
    expect(i32Info->runtimeElementSize && *i32Info->runtimeElementSize == 4,
           "i32 runtime element size");
  }

  const noria::TypeKindInfo* structInfo = noria::typeKindInfo(TypeKind::Struct);
  expect(structInfo != nullptr && !structInfo->runtimeElementSize, "struct has no fixed size");

  const noria::ImplementationTagInfo* hashmapInfo =
      noria::implementationTagInfo(ImplementationTag::Hashmap);
  expect(hashmapInfo != nullptr, "hashmap tag info exists");
  if (hashmapInfo != nullptr) {
    expectText(hashmapInfo->name, "hashmap", "hashmap tag name");
    expect(hashmapInfo->requiredOperations ==
               std::vector<RequiredOperation>{RequiredOperation::Equality, RequiredOperation::Hash},
           "hashmap required operations");
  }

  const noria::RequiredOperationInfo* hashInfo =
      noria::requiredOperationInfo(RequiredOperation::Hash);
  expect(hashInfo != nullptr, "hash operation info exists");
  if (hashInfo != nullptr) {
    expectText(hashInfo->name, "hash", "hash operation name");
    expect(hashInfo->supportedTypeKinds ==
               std::vector<TypeKind>{TypeKind::I32, TypeKind::Bool, TypeKind::Str},
           "hash supported type kinds");
  }

  if (failures != 0) {
    std::cerr << failures << " semantic table test(s) failed\n";
    return EXIT_FAILURE;
  }

  std::cout << "semantic table tests ok\n";
  return EXIT_SUCCESS;
}
