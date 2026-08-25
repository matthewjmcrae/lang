#include "noria/Codegen.hpp"
#include "noria/Compiler.hpp"
#include "noria/Diagnostic.hpp"
#include "noria/Monomorphize.hpp"
#include "noria/TypeChecker.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <type_traits>

namespace {

  int failures = 0;

  void expect(bool condition, const char* message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      ++failures;
    }
  }

  void expectCompileError(noria::StopAfter stopAfter, std::string_view source,
                          const char* message) {
    try {
      noria::compileSource(source, stopAfter);
      expect(false, message);
    } catch (const noria::CompileError&) {
    }
  }

} // namespace

int main() {
  static_assert(std::is_copy_constructible_v<noria::TypeChecker>);
  static_assert(std::is_copy_assignable_v<noria::TypeChecker>);
  static_assert(std::is_move_constructible_v<noria::TypeChecker>);
  static_assert(std::is_move_assignable_v<noria::TypeChecker>);
  static_assert(std::is_copy_constructible_v<noria::LLVMGenerator>);
  static_assert(std::is_copy_assignable_v<noria::LLVMGenerator>);
  static_assert(std::is_move_constructible_v<noria::LLVMGenerator>);
  static_assert(std::is_move_assignable_v<noria::LLVMGenerator>);
  static_assert(std::is_copy_constructible_v<noria::SpecializationCache>);
  static_assert(std::is_copy_assignable_v<noria::SpecializationCache>);
  static_assert(std::is_move_constructible_v<noria::SpecializationCache>);
  static_assert(std::is_move_assignable_v<noria::SpecializationCache>);

  constexpr std::string_view goodSource = R"(
fn main() -> i32 {
  return 42;
}
)";

  constexpr std::string_view typeInvalidSource = R"(
fn main() -> i32 {
  return missing;
}
)";

  constexpr std::string_view syntaxInvalidSource = R"(
fn main() -> i32 {
  let x: i32 = 1
  return x;
}
)";

  const noria::PipelineOutput typedOutput =
      noria::compileSource(goodSource, noria::StopAfter::Typed);
  expect(!typedOutput.module.functions.empty(), "Typed stop produces module");
  expect(typedOutput.LLVM.empty(), "Typed stop does not generate IR");

  const noria::PipelineOutput tokensOutput =
      noria::compileSource(goodSource, noria::StopAfter::Tokens);
  expect(!tokensOutput.tokens.empty(), "Tokens stop produces tokens");
  expect(tokensOutput.module.functions.empty(), "Tokens stop does not parse");

  const noria::PipelineOutput astOutput =
      noria::compileSource(typeInvalidSource, noria::StopAfter::Ast);
  expect(!astOutput.module.functions.empty(),
         "Ast stop parses source that fails type checking");
  expect(astOutput.LLVM.empty(), "Ast stop does not generate IR");

  const noria::PipelineOutput irOutput =
      noria::compileSource(goodSource, noria::StopAfter::Ir);
  expect(irOutput.LLVM.find("define i32 @main") != std::string::npos,
         "Ir stop generates main");
  constexpr std::string_view expectedMainIr = R"(define i32 @main() {
entry:
  ret i32 42
}

)";
  const std::size_t mainStart = irOutput.LLVM.find("define i32 @main()");
  expect(mainStart != std::string::npos && irOutput.LLVM.substr(mainStart) == expectedMainIr,
         "representative main IR remains byte-for-byte stable");

  expectCompileError(noria::StopAfter::Typed, typeInvalidSource,
                     "Typed stop throws on type error");
  expectCompileError(noria::StopAfter::Ast, syntaxInvalidSource,
                     "Ast stop throws on syntax error");

  if (failures != 0) {
    std::cerr << failures << " compiler facade test failure(s)\n";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
