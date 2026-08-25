#include "noria/AstClone.hpp"
#include "noria/Ast.hpp"
#include "noria/Types.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
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

  noria::ast::Module buildModule() {
    using namespace noria::ast;

    noria::SourceLocation loc{"clone_test", 1, 1};

    Module module;
    module.imports.push_back(ImportDecl{{"std", "generic_id"}, {ImportedName{"id", loc}}, loc});

    StructDecl box;
    box.name = "Box";
    box.location = loc;
    box.typeParams.push_back(TypeParameter{"T", loc});
    box.fields.push_back(StructField{"value", noria::Type::typeParam("T"), loc});
    module.structs.push_back(std::move(box));

    Function function;
    function.name = "wrap";
    function.returnType = noria::Type::i32();
    function.location = loc;
    function.parameters.push_back(Parameter{"input", noria::Type::i32(), loc});
    function.body.push_back(std::make_unique<LetStatement>(
        "value", noria::Type::i32(), std::make_unique<IdentifierExpression>("input", loc), loc));
    function.body.push_back(
        std::make_unique<ReturnStatement>(std::make_unique<IdentifierExpression>("value", loc),
                                          loc));
    module.functions.push_back(std::move(function));

    Function procedure;
    procedure.name = "notify";
    procedure.returnType = noria::Type::voidType();
    procedure.location = loc;
    procedure.body.push_back(std::make_unique<ReturnStatement>(nullptr, loc));
    module.functions.push_back(std::move(procedure));

    return module;
  }

} // namespace

int main() {
  noria::ast::Module original = buildModule();
  noria::ast::Module cloned = noria::ast::cloneModule(original);

  expect(cloned.imports.size() == 1, "clone preserves imports");
  expect(cloned.structs.size() == 1, "clone preserves structs");
  expect(cloned.functions.size() == 2, "clone preserves functions");
  expect(cloned.functions[0].body.size() == 2, "clone preserves function body");

  cloned.imports[0].names[0].name = "renamed_id";
  cloned.structs[0].fields[0].name = "renamed_value";
  cloned.functions[0].name = "renamed_wrap";

  auto* clonedLet = dynamic_cast<noria::ast::LetStatement*>(cloned.functions[0].body[0].get());
  auto* originalLet = dynamic_cast<noria::ast::LetStatement*>(original.functions[0].body[0].get());
  expect(clonedLet != nullptr && originalLet != nullptr, "clone keeps let statement kind");
  if (clonedLet == nullptr || originalLet == nullptr) {
    return EXIT_FAILURE;
  }

  auto* clonedIdentifier =
      dynamic_cast<noria::ast::IdentifierExpression*>(clonedLet->initializer.get());
  auto* originalIdentifier =
      dynamic_cast<noria::ast::IdentifierExpression*>(originalLet->initializer.get());
  expect(clonedIdentifier != nullptr && originalIdentifier != nullptr,
         "clone keeps identifier expression kind");
  if (clonedIdentifier == nullptr || originalIdentifier == nullptr) {
    return EXIT_FAILURE;
  }

  clonedIdentifier->name = "changed_input";

  expect(original.imports[0].names[0].name == "id", "import clone is independently mutable");
  expect(original.structs[0].fields[0].name == "value",
         "struct clone is independently mutable");
  expect(original.functions[0].name == "wrap", "function clone is independently mutable");
  expect(originalIdentifier->name == "input", "expression clone is independently mutable");
  const auto* clonedBareReturn =
      dynamic_cast<const noria::ast::ReturnStatement*>(cloned.functions[1].body[0].get());
  expect(clonedBareReturn != nullptr && !clonedBareReturn->expression,
         "clone preserves bare return statements");

  if (failures != 0) {
    std::cerr << failures << " AST clone test failure(s)\n";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
