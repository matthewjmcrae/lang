#include "noria/Codegen.hpp"
#include "noria/Compiler.hpp"
#include "noria/Diagnostic.hpp"
#include "noria/Monomorphize.hpp"
#include "noria/TypeChecker.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <type_traits>
#include <utility>

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

  void expectCompileErrorMessage(noria::StopAfter stopAfter, std::string_view source,
                                 std::string_view expected, const char* message) {
    try {
      noria::compileSource(source, stopAfter);
      expect(false, message);
    } catch (const noria::CompileError& error) {
      expect(error.what() == expected, message);
    }
  }

} // namespace

int main() {
  static_assert(!std::is_copy_constructible_v<noria::TypeChecker>);
  static_assert(!std::is_copy_assignable_v<noria::TypeChecker>);
  static_assert(std::is_move_constructible_v<noria::TypeChecker>);
  static_assert(std::is_move_assignable_v<noria::TypeChecker>);
  static_assert(!std::is_copy_constructible_v<noria::LLVMGenerator>);
  static_assert(!std::is_copy_assignable_v<noria::LLVMGenerator>);
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

  constexpr std::string_view voidProcedureSource = R"(
fn announce() -> void {
  return;
}

fn main() -> i32 {
  announce();
  return 0;
}
)";

  constexpr std::string_view missingReturnSource = R"(
fn status(enabled: bool) -> i32 {
  if enabled {
    return 7;
  }
}

fn main() -> i32 {
  return 0;
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

  constexpr std::string_view missingMainSource = "fn helper() -> i32 { return 0; }\n";
  constexpr std::string_view genericMainSource = "fn main<T>() -> i32 { return 0; }\n";
  constexpr std::string_view parameterizedMainSource =
      "fn main(argc: i32) -> i32 { return argc; }\n";
  constexpr std::string_view boolMainSource = "fn main() -> bool { return true; }\n";
  constexpr std::string_view f64MainSource = "fn main() -> f64 { return 1.0; }\n";
  constexpr std::string_view voidMainSource = "fn main() -> void { return; }\n";
  constexpr std::string_view strMainSource = "fn main() -> str { return \"x\"; }\n";
  constexpr std::string_view invalidInMultipleWaysMainSource =
      "fn main<T>(argc: i32) -> bool { return true; }\n";
  constexpr std::string_view duplicateMainSource = R"(fn main() -> i32 { return 0; }
fn main() -> i32 { return 0; }
)";
  constexpr std::string_view userPrintSource = R"(fn print(msg: str) -> void {
  return;
}

fn main() -> i32 {
  print("should not appear if overridden");
  return 0;
}
)";
  constexpr std::string_view unusedPrintlnSource = R"(fn println() -> void {
  return;
}

fn main() -> i32 {
  return 0;
}
)";
  constexpr std::string_view userGenericLenSource = R"(fn len<T>(value: T) -> i32 {
  return 0;
}

fn main() -> i32 {
  return 0;
}
)";
  constexpr std::string_view emptyTypedArraySource = R"(
fn main() -> i32 {
  let values: [i32] = [];
  return len(values);
}
)";
  constexpr std::string_view defaultArraySource = R"(
fn main() -> i32 {
  values: [i32];
  return len(values);
}
)";
  constexpr std::string_view defaultStrSource = R"(
fn main() -> i32 {
  s: str;
  return len(s);
}
)";
  constexpr std::string_view emptyUntypedArraySource = R"(
fn main() -> i32 {
  let values = [];
  return 0;
}
)";
  constexpr std::string_view checkedIntegerOpsSource = R"(
fn main() -> i32 {
  let zero: i32 = 0;
  let wide: i32 = 32;
  let quotient: i32 = 1 / zero;
  let shifted: i32 = 1 << wide;
  return quotient + shifted;
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
  expect(!astOutput.module.functions.empty(), "Ast stop parses source that fails type checking");
  expect(astOutput.LLVM.empty(), "Ast stop does not generate IR");

  const noria::PipelineOutput noMainAstOutput =
      noria::compileSource(missingMainSource, noria::StopAfter::Ast);
  expect(!noMainAstOutput.module.functions.empty(),
         "Ast stop accepts source without an entry point");

  const noria::PipelineOutput irOutput = noria::compileSource(goodSource, noria::StopAfter::Ir);
  expect(irOutput.LLVM.find("define i32 @main") != std::string::npos, "Ir stop generates main");
  constexpr std::string_view expectedMainIr = R"(define i32 @main() {
entry:
  ret i32 42
}

)";
  const std::size_t mainStart = irOutput.LLVM.find("define i32 @main()");
  expect(mainStart != std::string::npos && irOutput.LLVM.substr(mainStart) == expectedMainIr,
         "representative main IR remains byte-for-byte stable");

  const noria::PipelineOutput voidProcedureOutput =
      noria::compileSource(voidProcedureSource, noria::StopAfter::Ir);
  expect(voidProcedureOutput.LLVM.find("define void @announce()") != std::string::npos,
         "void procedure emits a void definition");
  expect(voidProcedureOutput.LLVM.find("call void @announce()") != std::string::npos,
         "void procedure call does not produce an SSA value");
  expect(voidProcedureOutput.LLVM.find("ret void") != std::string::npos,
         "bare return emits ret void");

  constexpr std::string_view checkedCastSource = R"(
fn main() -> i32 {
  let value: f64 = 1.5;
  return value as i32;
}
)";
  const noria::PipelineOutput checkedCastOutput =
      noria::compileSource(checkedCastSource, noria::StopAfter::Ir);
  const std::size_t lowerBoundCheck = checkedCastOutput.LLVM.find("fcmp ogt double");
  const std::size_t upperBoundCheck = checkedCastOutput.LLVM.find("fcmp olt double");
  const std::size_t trapBlock = checkedCastOutput.LLVM.find("cast.fail0:");
  const std::size_t successBlock = checkedCastOutput.LLVM.find("cast.ok0:");
  const std::size_t conversion = checkedCastOutput.LLVM.find("fptosi double");
  expect(lowerBoundCheck != std::string::npos && upperBoundCheck != std::string::npos,
         "f64-to-i32 casts emit ordered range checks");
  expect(trapBlock != std::string::npos && successBlock != std::string::npos,
         "f64-to-i32 casts emit success and trap blocks");
  expect(conversion != std::string::npos && lowerBoundCheck < conversion &&
             upperBoundCheck < conversion && trapBlock < conversion && successBlock < conversion,
         "f64-to-i32 conversion follows its range guard");
  expect(conversion != std::string::npos &&
             checkedCastOutput.LLVM.find("fptosi double", conversion + 1) == std::string::npos,
         "checked-cast fixture emits exactly one fptosi");

  const noria::PipelineOutput checkedIntegerOutput =
      noria::compileSource(checkedIntegerOpsSource, noria::StopAfter::Ir);
  const std::size_t divisorCheck = checkedIntegerOutput.LLVM.find("icmp ne i32");
  const std::size_t divisorTrap = checkedIntegerOutput.LLVM.find("integer.divisor.fail");
  const std::size_t division = checkedIntegerOutput.LLVM.find("sdiv i32");
  const std::size_t shiftCheck = checkedIntegerOutput.LLVM.find("icmp ult i32");
  const std::size_t shiftTrap = checkedIntegerOutput.LLVM.find("integer.shift.fail");
  const std::size_t shift = checkedIntegerOutput.LLVM.find("shl i32");
  expect(divisorCheck != std::string::npos && divisorTrap != std::string::npos &&
             division != std::string::npos && divisorCheck < division && divisorTrap < division,
         "integer division emits a zero-divisor guard before sdiv");
  expect(shiftCheck != std::string::npos && shiftTrap != std::string::npos &&
             shift != std::string::npos && shiftCheck < shift && shiftTrap < shift,
         "integer shifts emit a 0..31 count guard before shl");

  const noria::PipelineOutput emptyTypedArrayOutput =
      noria::compileSource(emptyTypedArraySource, noria::StopAfter::Ir);
  expect(emptyTypedArrayOutput.LLVM.find("call ptr @malloc(i64 8)") != std::string::npos,
         "typed empty array literals allocate an 8-byte header");
  expect(emptyTypedArrayOutput.LLVM.find("store i64 0") != std::string::npos,
         "typed empty array literals store a zero length");

  const noria::PipelineOutput defaultArrayOutput =
      noria::compileSource(defaultArraySource, noria::StopAfter::Ir);
  expect(defaultArrayOutput.LLVM.find("call ptr @malloc(i64 8)") != std::string::npos,
         "default-initialized arrays allocate an 8-byte header");
  expect(defaultArrayOutput.LLVM.find("store i64 0") != std::string::npos,
         "default-initialized arrays store a zero length");
  expect(defaultArrayOutput.LLVM.find("store ptr null") == std::string::npos,
         "default-initialized arrays must not store null");

  const noria::PipelineOutput defaultStrOutput =
      noria::compileSource(defaultStrSource, noria::StopAfter::Ir);
  expect(defaultStrOutput.LLVM.find("store ptr null") == std::string::npos,
         "default-initialized strings must not store null");
  expect(defaultStrOutput.LLVM.find("[1 x i8]") != std::string::npos,
         "default-initialized strings materialize an empty C string");

  expectCompileError(noria::StopAfter::Typed, typeInvalidSource, "Typed stop throws on type error");
  expectCompileError(noria::StopAfter::Ast, syntaxInvalidSource, "Ast stop throws on syntax error");
  expectCompileErrorMessage(
      noria::StopAfter::Typed, missingReturnSource,
      "2:1: typecheck: not all control-flow paths in function 'status' return",
      "Typed stop rejects a non-void fallthrough path");
  expectCompileErrorMessage(noria::StopAfter::Typed, missingMainSource,
                            "2:1: typecheck: missing entry point; expected 'fn main() -> i32'",
                            "Typed stop rejects missing main at end of file");
  expectCompileErrorMessage(
      noria::StopAfter::Ir, genericMainSource,
      "1:1: typecheck: entry point 'main' must not be generic; expected 'fn main() -> i32'",
      "Ir stop rejects generic main");
  expectCompileErrorMessage(
      noria::StopAfter::Typed, parameterizedMainSource,
      "1:1: typecheck: entry point 'main' must accept no parameters; expected 'fn main() -> i32'",
      "Typed stop rejects parameterized main");
  expectCompileErrorMessage(noria::StopAfter::Ir, boolMainSource,
                            "1:1: typecheck: entry point 'main' must return i32, got bool",
                            "Ir stop rejects bool main return type");
  expectCompileErrorMessage(noria::StopAfter::Ir, f64MainSource,
                            "1:1: typecheck: entry point 'main' must return i32, got f64",
                            "Ir stop rejects f64 main return type");
  expectCompileErrorMessage(noria::StopAfter::Ir, voidMainSource,
                            "1:1: typecheck: entry point 'main' must return i32, got void",
                            "Ir stop rejects void main return type");
  expectCompileErrorMessage(noria::StopAfter::Ir, strMainSource,
                            "1:1: typecheck: entry point 'main' must return i32, got str",
                            "Ir stop rejects str main return type");
  expectCompileErrorMessage(noria::StopAfter::Typed, emptyUntypedArraySource,
                            "3:16: typecheck: cannot infer element type of empty array literal",
                            "Typed stop still rejects an unannotated empty array literal");
  expectCompileErrorMessage(
      noria::StopAfter::Ir, invalidInMultipleWaysMainSource,
      "1:1: typecheck: entry point 'main' must not be generic; expected 'fn main() -> i32'",
      "Entry-point validation checks generic status before parameters and return type");
  expectCompileErrorMessage(noria::StopAfter::Typed, duplicateMainSource,
                            "2:1: typecheck: duplicate function 'main'",
                            "Type checking retains duplicate main diagnostic");
  expectCompileErrorMessage(noria::StopAfter::Typed, userPrintSource,
                            "1:1: typecheck: cannot define function 'print': name is a builtin",
                            "Type checking rejects a user function named after a called builtin");
  expectCompileErrorMessage(noria::StopAfter::Typed, unusedPrintlnSource,
                            "1:1: typecheck: cannot define function 'println': name is a builtin",
                            "Type checking rejects a user function named after an unused builtin");
  expectCompileErrorMessage(noria::StopAfter::Typed, userGenericLenSource,
                            "1:1: typecheck: cannot define function 'len': name is a builtin",
                            "Type checking rejects a generic function named after a builtin");

  noria::TypeChecker checker;
  try {
    checker.check(astOutput.module);
    expect(false, "checker reports invalid module");
  } catch (const noria::CompileError&) {
  }
  checker.check(typedOutput.module);
  noria::TypeChecker movedChecker(std::move(checker));
  movedChecker.check(typedOutput.module);

  noria::LLVMGenerator generator;
  generator.setFunctionSpecializationTypeArgs({});
  noria::LLVMGenerator movedGenerator(std::move(generator));
  const std::string movedIr = movedGenerator.generate(typedOutput.module);
  expect(movedIr.find("define i32 @main") != std::string::npos, "moved generator remains usable");

  if (failures != 0) {
    std::cerr << failures << " compiler facade test failure(s)\n";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
