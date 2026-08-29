#include "noria/AstClone.hpp"
#include "noria/Codegen.hpp"
#include "noria/Compiler.hpp"
#include "noria/Diagnostic.hpp"
#include "noria/Monomorphize.hpp"
#include "noria/Token.hpp"
#include "noria/TypeChecker.hpp"

#include <array>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
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

  bool instructionNamesAreUnique(std::string_view llvmIr) {
    std::size_t position = 0;
    while ((position = llvmIr.find("define ", position)) != std::string_view::npos) {
      const std::size_t bodyStart = llvmIr.find('{', position);
      if (bodyStart == std::string_view::npos) {
        return false;
      }

      std::size_t bodyEnd = bodyStart + 1;
      int depth = 1;
      while (bodyEnd < llvmIr.size() && depth > 0) {
        if (llvmIr[bodyEnd] == '{') {
          ++depth;
        } else if (llvmIr[bodyEnd] == '}') {
          --depth;
        }
        ++bodyEnd;
      }
      if (depth != 0) {
        return false;
      }

      const std::string_view body = llvmIr.substr(bodyStart, bodyEnd - bodyStart);
      std::unordered_set<std::string> names;
      std::size_t lineStart = 0;
      while (lineStart < body.size()) {
        std::size_t lineEnd = body.find('\n', lineStart);
        if (lineEnd == std::string_view::npos) {
          lineEnd = body.size();
        }
        std::string_view line = body.substr(lineStart, lineEnd - lineStart);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
          line.remove_prefix(1);
        }
        if (!line.empty() && line.front() == '%') {
          const std::size_t equals = line.find(" = ");
          if (equals != std::string_view::npos) {
            const std::string name(line.substr(0, equals));
            if (!names.insert(name).second) {
              return false;
            }
          }
        }
        lineStart = lineEnd + 1;
      }

      position = bodyEnd;
    }
    return true;
  }

  std::size_t countOwnedAlloca(std::string_view llvmIr, std::string_view localName,
                               bool bareOnly) {
    const std::string prefix = "%" + std::string(localName) + ".owned";
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = llvmIr.find(prefix, position)) != std::string_view::npos) {
      std::size_t cursor = position + prefix.size();
      bool digits = false;
      while (cursor < llvmIr.size() &&
             std::isdigit(static_cast<unsigned char>(llvmIr[cursor])) != 0) {
        digits = true;
        ++cursor;
      }
      const bool matchesForm = bareOnly ? !digits : digits;
      if (matchesForm && llvmIr.compare(cursor, 9, " = alloca") == 0) {
        ++count;
      }
      position = cursor;
    }
    return count;
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

  constexpr std::string_view functionKeywordSource = R"(
UTIL Increment(VALUE: I32) -> I32 {
  RETURN VALUE + 1;
}

HELPER Double(VALUE: I32) -> I32 {
  RETURN VALUE + VALUE;
}

RECFN Sum_To(VALUE: I32) -> I32 {
  IF VALUE == 0 {
    RETURN 0;
  }
  RETURN VALUE + SUM_TO(VALUE - 1);
}

FN MAIN() -> I32 {
  RETURN DOUBLE(INCREMENT(SUM_TO(3)));
}
)";

  constexpr std::string_view mixedCaseStringSource = R"(
FN MAIN() -> STR {
  RETURN "MiXeD Case";
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

  constexpr std::string_view missingMainSource = "helper missing_entry() -> i32 { return 0; }\n";
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
  constexpr std::string_view managedAutoFreeSource = R"(
fn main() -> i32 {
  let s: str = "a";
  s = s + "b";
  let a: [i32] = [1, 2];
  let b: [i32] = a;
  return len(s) + len(b);
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

  const noria::PipelineOutput functionKeywordTokens =
      noria::compileSource(functionKeywordSource, noria::StopAfter::Tokens);
  constexpr std::array<std::string_view, 4> functionKeywordSpellings =
      {"fn", "util", "helper", "recfn"};
  for (const std::string_view spelling : functionKeywordSpellings) {
    bool found = false;
    for (const noria::Token& token : functionKeywordTokens.tokens) {
      if (token.text == spelling && token.kind == noria::TokenKind::Fn) {
        found = true;
        break;
      }
    }
    expect(found, "function declaration spelling lexes as fn");
  }
  bool foundCanonicalIdentifier = false;
  for (const noria::Token& token : functionKeywordTokens.tokens) {
    if (token.kind == noria::TokenKind::Identifier && token.text == "increment") {
      foundCanonicalIdentifier = true;
      break;
    }
  }
  expect(foundCanonicalIdentifier, "mixed-case identifiers lex as lowercase names");

  const noria::PipelineOutput functionKeywordOutput =
      noria::compileSource(functionKeywordSource, noria::StopAfter::Ir);
  expect(functionKeywordOutput.module.functions.size() == 4,
         "all function declaration spellings parse into ordinary functions");
  expect(functionKeywordOutput.LLVM.find("define i32 @sum_to") != std::string::npos,
         "recfn declaration follows the ordinary code-generation path");

  const noria::PipelineOutput mixedCaseStringTokens =
      noria::compileSource(mixedCaseStringSource, noria::StopAfter::Tokens);
  bool preservedString = false;
  for (const noria::Token& token : mixedCaseStringTokens.tokens) {
    if (token.kind == noria::TokenKind::String && token.text == "MiXeD Case") {
      preservedString = true;
      break;
    }
  }
  expect(preservedString, "string literal casing is preserved while names are normalized");

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

  const noria::PipelineOutput managedAutoFreeOutput =
      noria::compileSource(managedAutoFreeSource, noria::StopAfter::Ir);
  expect(managedAutoFreeOutput.LLVM.find("call void @__noria.rt.drop_str") != std::string::npos,
         "string reassignment emits drop_str for the previous value");
  expect(managedAutoFreeOutput.LLVM.find("call void @free") != std::string::npos,
         "managed array copies emit free on reassignment or scope exit");

  constexpr std::string_view siblingManagedStrSource = R"(
fn main() -> i32 {
  if true {
    let s = "a" + "b";
    return len(s);
  } else {
    s: str = "c" + "d";
    return len(s);
  }
}
)";
  const noria::PipelineOutput siblingManagedStrOutput =
      noria::compileSource(siblingManagedStrSource, noria::StopAfter::Ir);
  expect(instructionNamesAreUnique(siblingManagedStrOutput.LLVM),
         "sibling managed str locals emit unique instruction names");
  expect(countOwnedAlloca(siblingManagedStrOutput.LLVM, "s", true) == 0,
         "sibling managed str locals do not emit bare %s.owned");
  expect(countOwnedAlloca(siblingManagedStrOutput.LLVM, "s", false) == 2,
         "sibling managed str locals emit two uniquified %s.ownedN flags");

  constexpr std::string_view siblingDefaultManagedSource = R"(
fn main() -> i32 {
  if true {
    let s: str;
    return len(s);
  } else {
    str: s;
    return len(s);
  }
}
)";
  const noria::PipelineOutput siblingDefaultManagedOutput =
      noria::compileSource(siblingDefaultManagedSource, noria::StopAfter::Ir);
  expect(instructionNamesAreUnique(siblingDefaultManagedOutput.LLVM),
         "default-initialized sibling managed locals emit unique instruction names");
  expect(countOwnedAlloca(siblingDefaultManagedOutput.LLVM, "s", true) == 0,
         "default-initialized sibling managed locals do not emit bare %s.owned");
  expect(countOwnedAlloca(siblingDefaultManagedOutput.LLVM, "s", false) == 2,
         "default-initialized sibling managed locals emit two uniquified flags");

  constexpr std::string_view nestedManagedShadowSource = R"(
fn main() -> i32 {
  let s = "a" + "b";
  if true {
    let s = "c" + "d" + "e";
    return len(s);
  }
  return len(s);
}
)";
  const noria::PipelineOutput nestedManagedShadowOutput =
      noria::compileSource(nestedManagedShadowSource, noria::StopAfter::Ir);
  expect(instructionNamesAreUnique(nestedManagedShadowOutput.LLVM),
         "nested inferred managed shadow emits unique instruction names");
  expect(countOwnedAlloca(nestedManagedShadowOutput.LLVM, "s", true) == 0,
         "nested inferred managed shadow does not emit bare %s.owned");
  expect(countOwnedAlloca(nestedManagedShadowOutput.LLVM, "s", false) == 2,
         "nested inferred managed shadow emits two uniquified flags");

  constexpr std::string_view siblingManagedArraySource = R"(
fn main() -> i32 {
  if true {
    let a: [i32] = [1, 2];
    return len(a);
  } else {
    [i32]: a = [3, 4, 5];
    return len(a);
  }
}
)";
  const noria::PipelineOutput siblingManagedArrayOutput =
      noria::compileSource(siblingManagedArraySource, noria::StopAfter::Ir);
  expect(instructionNamesAreUnique(siblingManagedArrayOutput.LLVM),
         "sibling managed array locals emit unique instruction names");
  expect(countOwnedAlloca(siblingManagedArrayOutput.LLVM, "a", true) == 0,
         "sibling managed array locals do not emit bare %a.owned");
  expect(countOwnedAlloca(siblingManagedArrayOutput.LLVM, "a", false) == 2,
         "sibling managed array locals emit two uniquified %a.ownedN flags");

  constexpr std::string_view siblingManagedStructSource = R"(
struct Holder {
  text: str;
}

fn main() -> i32 {
  if true {
    let h: Holder = Holder { text: "a" + "b" };
    return len(h.text);
  } else {
    h: Holder = Holder { text: "c" + "d" };
    return len(h.text);
  }
}
)";
  const noria::PipelineOutput siblingManagedStructOutput =
      noria::compileSource(siblingManagedStructSource, noria::StopAfter::Ir);
  expect(instructionNamesAreUnique(siblingManagedStructOutput.LLVM),
         "sibling managed struct locals emit unique instruction names");
  expect(countOwnedAlloca(siblingManagedStructOutput.LLVM, "h", true) == 0,
         "sibling managed struct locals do not emit bare %h.owned");
  expect(countOwnedAlloca(siblingManagedStructOutput.LLVM, "h", false) == 2,
         "sibling managed struct locals emit two uniquified %h.ownedN flags");

  constexpr std::string_view siblingManagedGenericSource = R"(
fn id<T>(value: T) -> T {
  return value;
}

fn main() -> i32 {
  if true {
    let s: str = id("a" + "b");
    return len(s);
  } else {
    str: s = id("c" + "d");
    return len(s);
  }
}
)";
  const noria::PipelineOutput siblingManagedGenericOutput =
      noria::compileSource(siblingManagedGenericSource, noria::StopAfter::Ir);
  expect(instructionNamesAreUnique(siblingManagedGenericOutput.LLVM),
         "sibling generic-managed locals emit unique instruction names");
  expect(countOwnedAlloca(siblingManagedGenericOutput.LLVM, "s", true) == 0,
         "sibling generic-managed locals do not emit bare %s.owned");
  expect(countOwnedAlloca(siblingManagedGenericOutput.LLVM, "s", false) == 2,
         "sibling generic-managed locals emit two uniquified %s.ownedN flags");

  constexpr std::string_view sequentialSiblingManagedSource = R"(
fn main() -> i32 {
  let total: i32 = 0;
  if true {
    let s: str = "a" + "b";
    total = total + len(s);
  }
  while false {
    s: str = "c" + "d";
    total = total + len(s);
  }
  if true {
    str: s = "e" + "f";
    total = total + len(s);
  }
  return total;
}
)";
  const noria::PipelineOutput sequentialSiblingManagedOutput =
      noria::compileSource(sequentialSiblingManagedSource, noria::StopAfter::Ir);
  expect(instructionNamesAreUnique(sequentialSiblingManagedOutput.LLVM),
         "sequential if/while managed siblings emit unique instruction names");
  expect(countOwnedAlloca(sequentialSiblingManagedOutput.LLVM, "s", true) == 0,
         "sequential managed siblings do not emit bare %s.owned");
  expect(countOwnedAlloca(sequentialSiblingManagedOutput.LLVM, "s", false) == 3,
         "sequential managed siblings emit three uniquified %s.ownedN flags");

  constexpr std::string_view shadowManagedParamSource = R"(
fn length(s: str) -> i32 {
  if true {
    let s: str = "a" + "b";
    return len(s);
  }
  return len(s);
}

fn main() -> i32 {
  return length("x");
}
)";
  const noria::PipelineOutput shadowManagedParamOutput =
      noria::compileSource(shadowManagedParamSource, noria::StopAfter::Ir);
  expect(instructionNamesAreUnique(shadowManagedParamOutput.LLVM),
         "managed parameter shadowing emits unique instruction names");
  expect(countOwnedAlloca(shadowManagedParamOutput.LLVM, "s", true) == 1,
         "managed parameters keep a bare %s.owned flag");
  expect(countOwnedAlloca(shadowManagedParamOutput.LLVM, "s", false) == 1,
         "shadowing managed locals emit one uniquified %s.ownedN flag");

  constexpr std::string_view siblingUnmanagedSource = R"(
fn main() -> i32 {
  if true {
    let n: i32 = 1;
    return n;
  } else {
    n: i32 = 2;
    return n;
  }
}
)";
  const noria::PipelineOutput siblingUnmanagedOutput =
      noria::compileSource(siblingUnmanagedSource, noria::StopAfter::Ir);
  expect(instructionNamesAreUnique(siblingUnmanagedOutput.LLVM),
         "sibling unmanaged locals still emit unique instruction names");
  expect(countOwnedAlloca(siblingUnmanagedOutput.LLVM, "n", true) == 0 &&
             countOwnedAlloca(siblingUnmanagedOutput.LLVM, "n", false) == 0,
         "sibling unmanaged locals do not emit ownership flags");

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
    noria::ast::Module astModule = noria::ast::cloneModule(astOutput.module);
    checker.check(astModule);
    expect(false, "checker reports invalid module");
  } catch (const noria::CompileError&) {
  }
  noria::ast::Module typedModule = noria::ast::cloneModule(typedOutput.module);
  checker.check(typedModule);
  noria::TypeChecker movedChecker(std::move(checker));
  movedChecker.check(typedModule);

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
