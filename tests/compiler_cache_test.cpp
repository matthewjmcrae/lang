#include "noria/CompilerCache.hpp"
#include "noria/Types.hpp"

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

namespace {

  int failures = 0;

  void expect(bool condition, const char* message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      ++failures;
    }
  }

  noria::SourceLocation loc() {
    return noria::SourceLocation{"compiler_cache_test", 1, 1};
  }

  noria::ast::Function makeFunction(std::string name, std::size_t localCount) {
    noria::ast::Function function;
    function.name = std::move(name);
    function.returnType = noria::Type::i32();
    function.location = loc();
    function.parameters.push_back(noria::ast::Parameter{"x", noria::Type::i32(), loc()});
    for (std::size_t index{}; index < localCount; ++index) {
      function.body.push_back(std::make_unique<noria::ast::LetStatement>(
          "v" + std::to_string(index), noria::Type::i32(),
          std::make_unique<noria::ast::IntegerLiteral>(static_cast<std::int64_t>(index), loc()),
          loc()));
    }
    function.body.push_back(std::make_unique<noria::ast::ReturnStatement>(
        std::make_unique<noria::ast::IdentifierExpression>("x", loc()), loc()));
    return function;
  }

  noria::ast::StructDecl makeStruct(std::string name, std::size_t fieldCount) {
    noria::ast::StructDecl decl;
    decl.name = std::move(name);
    decl.location = loc();
    for (std::size_t index{}; index < fieldCount; ++index) {
      decl.fields.push_back(noria::ast::StructField{"f" + std::to_string(index),
                                                    noria::Type::i32(), loc()});
    }
    return decl;
  }

} // namespace

int main() {
  noria::CompilerCache cache;

  const noria::ast::Function smallFunction = makeFunction("small$s.i32", 0);
  cache.storeStdlibFunctionSpecialization("fn|std::small|small$s.i32", smallFunction,
                                          {noria::Type::i32()});
  expect(cache.stdlibSpecializationCount() == 0,
         "sub-1KiB function specialization is not admitted");
  expect(!cache.cloneStdlibFunctionSpecialization("fn|std::small|small$s.i32"),
         "rejected function specialization is not retrievable");

  const noria::ast::Function largeFunction = makeFunction("large$s.i32", 64);
  cache.storeStdlibFunctionSpecialization("fn|std::large|large$s.i32", largeFunction,
                                          {noria::Type::i32()});
  expect(cache.stdlibSpecializationCount() == 1,
         "large function specialization is admitted");
  std::optional<noria::CachedFunctionSpecialization> firstFunction =
      cache.cloneStdlibFunctionSpecialization("fn|std::large|large$s.i32");
  expect(firstFunction.has_value(), "admitted function specialization is retrievable");
  if (firstFunction) {
    firstFunction->function.name = "mutated";
  }
  std::optional<noria::CachedFunctionSpecialization> secondFunction =
      cache.cloneStdlibFunctionSpecialization("fn|std::large|large$s.i32");
  expect(secondFunction && secondFunction->function.name == "large$s.i32",
         "cached function retrieval returns independent clones");

  const noria::ast::StructDecl smallStruct = makeStruct("Small$s.i32", 7);
  cache.storeStdlibStructSpecialization("struct|std::small|Small$s.i32", smallStruct);
  expect(cache.stdlibSpecializationCount() == 1,
         "struct specialization below eight fields is not admitted");
  expect(!cache.cloneStdlibStructSpecialization("struct|std::small|Small$s.i32"),
         "rejected struct specialization is not retrievable");

  const noria::ast::StructDecl largeStruct = makeStruct("Large$s.i32", 8);
  cache.storeStdlibStructSpecialization("struct|std::large|Large$s.i32", largeStruct);
  expect(cache.stdlibSpecializationCount() == 2,
         "struct specialization with eight fields is admitted");
  std::optional<noria::ast::StructDecl> firstStruct =
      cache.cloneStdlibStructSpecialization("struct|std::large|Large$s.i32");
  expect(firstStruct.has_value(), "admitted struct specialization is retrievable");
  if (firstStruct) {
    firstStruct->fields[0].name = "mutated";
  }
  std::optional<noria::ast::StructDecl> secondStruct =
      cache.cloneStdlibStructSpecialization("struct|std::large|Large$s.i32");
  expect(secondStruct && secondStruct->fields[0].name == "f0",
         "cached struct retrieval returns independent clones");

  if (failures != 0) {
    std::cerr << failures << " compiler cache test failure(s)\n";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
