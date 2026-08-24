#include "noria/Compiler.hpp"
#include "noria/Diagnostic.hpp"
#include "noria/Monomorphize.hpp"
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

  void expectEqual(const std::string& actual, const std::string& expected, const char* message) {
    if (actual != expected) {
      std::cerr << "FAIL: " << message << " (expected '" << expected << "', got '" << actual
                << "')\n";
      ++failures;
    }
  }

  std::size_t countSpecializations(const noria::ast::Module& module, std::string_view prefix) {
    std::size_t count = 0;
    for (const auto& function : module.functions) {
      if (function.name.rfind(prefix, 0) == 0 && function.typeParams.empty() &&
          function.name != prefix) {
        ++count;
      }
    }
    return count;
  }

  std::size_t countDefines(const std::string& llvmIr, std::string_view name) {
    const std::string suffix = "@" + std::string(name) + "(";
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = llvmIr.find(suffix, position)) != std::string::npos) {
      const std::size_t lineStart = llvmIr.rfind('\n', position);
      const std::size_t searchFrom = lineStart == std::string::npos ? 0 : lineStart + 1;
      if (llvmIr.compare(searchFrom, 7, "define ") == 0) {
        ++count;
      }
      ++position;
    }
    return count;
  }

} // namespace

int main() {
  using noria::Type;

  expectEqual(noria::mangleType(Type::i32()), "s.i32", "mangle scalar i32");
  expectEqual(noria::mangleType(Type::structType("i32")), "st.i32", "mangle struct i32");
  expect(noria::mangleType(Type::i32()) != noria::mangleType(Type::structType("i32")),
         "scalar i32 and struct i32 mangle distinctly");

  expectEqual(noria::mangleSpecialization("id", {Type::i32()}), "id$s.i32",
              "mangle i32 specialization");
  expectEqual(noria::mangleSpecialization("id", {Type::boolean()}), "id$s.bool",
              "mangle bool specialization");
  expectEqual(noria::mangleSpecialization("id", {Type::array(Type::i32())}), "id$arr.s.i32",
              "mangle array specialization");
  expectEqual(noria::mangleSpecialization("wrap", {Type::structType("Point")}), "wrap$st.Point",
              "mangle struct specialization");
  expectEqual(noria::mangleSpecialization("tag", {Type::structType("i32")}), "tag$st.i32",
              "mangle struct named i32 specialization");
  expect(noria::mangleSpecialization("tag", {Type::i32()}) !=
             noria::mangleSpecialization("tag", {Type::structType("i32")}),
         "scalar and struct i32 specializations mangle distinctly");
  expect(noria::mangleSpecialization("id", {Type::i32()}) ==
             noria::mangleSpecialization("id", {Type::i32()}),
         "mangling is stable");

  noria::Substitution substitution;
  substitution.emplace("T", Type::i32());
  expect(noria::substitute(Type::typeParam("T"), substitution) == Type::i32(),
         "substitute scalar binding");
  expect(noria::substitute(Type::i32(), substitution) == Type::i32(),
         "substitute unrelated type");
  expect(noria::substitute(Type::array(Type::typeParam("T")), substitution) ==
             Type::array(Type::i32()),
         "substitute nested array type parameter");

  constexpr std::string_view reuseSource = R"(
fn id<T>(x: T) -> T {
  return x;
}

fn main() -> i32 {
  return id(1) + id(2);
}
)";

  const noria::CompileOutput reuseOutput =
      noria::compileSource(reuseSource, noria::StopAfter::Ir);
  expect(countDefines(reuseOutput.llvmIr, "id$s.i32") == 1,
         "same concrete call twice yields one define");

  constexpr std::string_view twoTypesSource = R"(
fn id<T>(x: T) -> T {
  return x;
}

fn main() -> i32 {
  let a: i32 = id(1);
  let b: bool = id(true);
  if b {
    return a;
  }
  return 0;
}
)";

  const noria::CompileOutput twoTypesOutput =
      noria::compileSource(twoTypesSource, noria::StopAfter::Typed);
  expect(countSpecializations(twoTypesOutput.module, "id") == 2,
         "two distinct type arguments yield two specializations");
  expect(
      countDefines(noria::compileSource(twoTypesSource, noria::StopAfter::Ir).llvmIr, "id$s.i32") ==
          1,
      "i32 specialization is emitted once");
  expect(countDefines(noria::compileSource(twoTypesSource, noria::StopAfter::Ir).llvmIr,
                       "id$s.bool") == 1,
         "bool specialization is emitted once");

  constexpr std::string_view collisionSource = R"(
struct i32 { x: i32; }

fn tag<T>(x: T) -> i32 {
  return 0;
}

fn main() -> i32 {
  return tag(1) + tag(i32 { x: 1 });
}
)";

  const noria::CompileOutput collisionOutput =
      noria::compileSource(collisionSource, noria::StopAfter::Typed);
  expect(countSpecializations(collisionOutput.module, "tag") == 2,
         "scalar i32 and struct i32 produce two tag specializations");
  expect(countDefines(noria::compileSource(collisionSource, noria::StopAfter::Ir).llvmIr,
                       "tag$s.i32") == 1,
         "scalar tag specialization is emitted");
  expect(countDefines(noria::compileSource(collisionSource, noria::StopAfter::Ir).llvmIr,
                       "tag$st.i32") == 1,
         "struct tag specialization is emitted");

  constexpr std::string_view unusedTemplateSource = R"(
fn unused<T>(x: T) -> T {
  return x;
}

fn main() -> i32 {
  return 0;
}
)";

  const noria::CompileOutput unusedOutput =
      noria::compileSource(unusedTemplateSource, noria::StopAfter::Typed);
  expect(countSpecializations(unusedOutput.module, "unused") == 0,
         "uncalled template yields no specializations");

  constexpr std::string_view structBoxSource = R"(
struct Box<T> {
  value: T;
}

fn main() -> i32 {
  let b: Box<i32> = Box<i32> { value: 42 };
  return b.value;
}
)";

  const noria::CompileOutput structBoxOutput =
      noria::compileSource(structBoxSource, noria::StopAfter::Ir);
  expect(structBoxOutput.llvmIr.find("%Box$s.i32 = type") != std::string::npos,
         "generic struct specialization emits concrete struct type");
  expect(structBoxOutput.llvmIr.find("struct Box<T>") == std::string::npos,
         "template struct is not emitted in IR");

  constexpr std::string_view unusedStructTemplateSource = R"(
struct Box<T> {
  value: T;
}

fn main() -> i32 {
  return 0;
}
)";

  const noria::CompileOutput unusedStructOutput =
      noria::compileSource(unusedStructTemplateSource, noria::StopAfter::Ir);
  expect(unusedStructOutput.llvmIr.find("%Box = type") == std::string::npos,
         "uncalled generic struct template is not emitted in IR");

  expectEqual(noria::mangleType(Type::structType("Box", {Type::i32()})), "st.Box$s.i32",
              "mangle applied struct type");

  expectEqual(noria::mangleType(Type::implementationTag(noria::ImplementationTag::Arr)), "tag.arr",
              "mangle implementation tag arr");
  expect(noria::mangleType(Type::implementationTag(noria::ImplementationTag::Arr)) !=
             noria::mangleType(Type::structType("arr")),
         "implementation tag arr is distinct from struct arr");
  expect(noria::mangleType(Type::implementationTag(noria::ImplementationTag::Arr)) !=
             noria::mangleType(Type::array(Type::i32())),
         "implementation tag arr is distinct from array i32 mangle prefix");

  expect(noria::mangleSpecialization(
             "Box", {Type::i32(), Type::implementationTag(noria::ImplementationTag::Arr)}) !=
             noria::mangleSpecialization(
                 "Box", {Type::i32(), Type::implementationTag(noria::ImplementationTag::List)}),
         "distinct implementation tags yield distinct specializations");

  expect(noria::substitute(Type::implementationTag(noria::ImplementationTag::Bst), substitution) ==
             Type::implementationTag(noria::ImplementationTag::Bst),
         "substitution leaves implementation tags unchanged");

  constexpr std::string_view implTagDistinctSource = R"(
struct Box<T, I> {
  value: T;
}

fn main() -> i32 {
  let a: Box<i32, arr> = Box<i32, arr> { value: 1 };
  let b: Box<i32, list> = Box<i32, list> { value: 2 };
  return a.value + b.value;
}
)";

  const noria::CompileOutput implTagDistinctOutput =
      noria::compileSource(implTagDistinctSource, noria::StopAfter::Ir);
  expect(implTagDistinctOutput.llvmIr.find("%Box$s.i32$tag.arr = type") != std::string::npos,
         "arr-tagged specialization is emitted");
  expect(implTagDistinctOutput.llvmIr.find("%Box$s.i32$tag.list = type") != std::string::npos,
         "list-tagged specialization is emitted");

  constexpr std::string_view implSelectSource = R"(
struct Box<T, I> {
  value: T;
}

fn kind<T, I>(b: Box<T, I>) -> i32 impl arr {
  return 1;
}

fn kind<T, I>(b: Box<T, I>) -> i32 impl list {
  return 2;
}

fn main() -> i32 {
  let a: Box<i32, arr> = Box<i32, arr> { value: 1 };
  let b: Box<i32, list> = Box<i32, list> { value: 2 };
  return kind(a) + kind(b);
}
)";

  const noria::CompileOutput implSelectOutput =
      noria::compileSource(implSelectSource, noria::StopAfter::Ir);
  expect(countDefines(implSelectOutput.llvmIr, "kind$s.i32$tag.arr") == 1,
         "arr-tagged kind specialization is emitted");
  expect(countDefines(implSelectOutput.llvmIr, "kind$s.i32$tag.list") == 1,
         "list-tagged kind specialization is emitted");

  noria::SpecializationCache cache;
  noria::ast::Module seededModule;
  noria::ast::Function existing;
  existing.name = "seed$s.i32";
  existing.returnType = noria::Type::i32();
  seededModule.functions.push_back(std::move(existing));
  cache.seedFromModule(seededModule);
  expect(cache.hasFunction("seed$s.i32"), "cache seed records existing specialization");
  expect(!cache.hasFunction("missing$s.i32"), "cache seed does not invent specializations");

  noria::SpecializationCache linkCache;
  bool linkCycleDetected = false;
  try {
    linkCache.link("b$s.i32", "a$s.i32", noria::SourceLocation{});
    linkCache.link("a$s.i32", "b$s.i32", noria::SourceLocation{});
  } catch (const noria::CompileError& error) {
    linkCycleDetected =
        std::string(error.what()).find("recursive generic specialization: a$s.i32 -> b$s.i32 -> a$s.i32") !=
        std::string::npos;
  }
  expect(linkCycleDetected, "dependency link detects ancestor cycle");

  constexpr std::string_view acyclicChainSource = R"(
fn f0<T>(x: T) -> i32 { return f1(x); }
fn f1<T>(x: T) -> i32 { return f2(x); }
fn f2<T>(x: T) -> i32 { return f3(x); }
fn f3<T>(x: T) -> i32 { return f4(x); }
fn f4<T>(x: T) -> i32 { return f5(x); }
fn f5<T>(x: T) -> i32 { return f6(x); }
fn f6<T>(x: T) -> i32 { return f7(x); }
fn f7<T>(x: T) -> i32 { return f8(x); }
fn f8<T>(x: T) -> i32 { return f9(x); }
fn f9<T>(x: T) -> i32 { return 0; }
fn main() -> i32 { return f0(1); }
)";

  bool acyclicChainCompiles = false;
  try {
    noria::compileSource(acyclicChainSource, noria::StopAfter::Ir);
    acyclicChainCompiles = true;
  } catch (const noria::CompileError&) {
    acyclicChainCompiles = false;
  }
  expect(acyclicChainCompiles, "acyclic specialization chain compiles");

  constexpr std::string_view growingTypeSource = R"(
fn grow<T>(x: T) -> i32 { return shrink(x); }
fn shrink<T>(x: T) -> i32 { return grow([x]); }
fn main() -> i32 { return grow(1); }
)";

  bool growingLimitDetected = false;
  try {
    noria::compileSource(growingTypeSource, noria::StopAfter::Ir);
  } catch (const noria::CompileError& error) {
    growingLimitDetected =
        std::string(error.what()).find("specialization expansion limit exceeded") != std::string::npos;
  }
  expect(growingLimitDetected, "growing specialization chain hits expansion limit");

  if (failures != 0) {
    std::cerr << failures << " generics test(s) failed\n";
    return EXIT_FAILURE;
  }

  std::cout << "generics tests ok\n";
  return EXIT_SUCCESS;
}
