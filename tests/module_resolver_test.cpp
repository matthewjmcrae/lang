#include "noria/Compiler.hpp"
#include "noria/CompilerCache.hpp"
#include "noria/Diagnostic.hpp"
#include "noria/Lexer.hpp"
#include "noria/ModuleResolver.hpp"
#include "noria/Parser.hpp"

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>

namespace {

  int failures = 0;

  void expect(bool condition, const char* message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      ++failures;
    }
  }

  void expectThrowsContains(const std::function<void()>& action, const char* message,
                            const std::string& expected) {
    try {
      action();
      expect(false, message);
    } catch (const noria::CompileError& error) {
      if (std::string(error.what()).find(expected) == std::string::npos) {
        std::cerr << "FAIL: " << message << " (missing '" << expected << "')\n";
        ++failures;
      }
    }
  }

  class MemoryModuleSourceProvider final : public noria::ModuleSourceProvider {
  public:
    void addModule(std::string modulePath, std::string source) {
      sources_.emplace(std::move(modulePath), std::move(source));
    }

    std::optional<std::string> loadModuleSource(const std::string& modulePath) override {
      ++loadCounts_[modulePath];
      const auto iterator = sources_.find(modulePath);
      if (iterator == sources_.end()) {
        return std::nullopt;
      }
      return iterator->second;
    }

    std::size_t loadCount(const std::string& modulePath) const {
      const auto iterator = loadCounts_.find(modulePath);
      if (iterator == loadCounts_.end()) {
        return 0;
      }
      return iterator->second;
    }

  private:
    std::unordered_map<std::string, std::string> sources_;
    std::unordered_map<std::string, std::size_t> loadCounts_;
  };

  noria::ast::Module parseModule(const std::string& source) {
    noria::Lexer lexer(source);
    const std::vector<noria::Token> tokens = lexer.lex();
    noria::Parser parser(tokens);
    return parser.parseModule();
  }

  std::size_t countFunction(const noria::ast::Module& module, const std::string& name) {
    std::size_t count{};
    for (const auto& function : module.functions) {
      if (function.name == name) {
        ++count;
      }
    }
    return count;
  }

  const noria::ast::Function* findFunction(const noria::ast::Module& module,
                                           const std::string& name) {
    for (const auto& function : module.functions) {
      if (function.name == name) {
        return &function;
      }
    }
    return nullptr;
  }

  bool hasDefaultImplementation(const noria::Type& type, noria::ImplementationTag tag) {
    return !type.typeArguments().empty() &&
           type.typeArguments().back().kind() == noria::TypeKind::ImplTag &&
           type.typeArguments().back().implementationTagValue() == tag;
  }

} // namespace

int main() {
  MemoryModuleSourceProvider provider;

  provider.addModule("std::a", R"(
import std::b::{b_fn};
fn a_fn() -> i32 { return b_fn(); }
)");
  provider.addModule("std::b", R"(
import std::a::{a_fn};
fn b_fn() -> i32 { return a_fn(); }
)");

  expectThrowsContains(
      [&] {
        noria::resolveImports(parseModule(R"(import std::b::{b_fn}; fn main() -> i32 { return b_fn(); })"),
                              noria::CompileOptions{}, provider);
      },
      "cycle diagnostic names both modules",
      "std::b:2:1: import: import cycle detected: std::b -> std::a -> std::b");

  MemoryModuleSourceProvider diamondProvider;
  diamondProvider.addModule("std::d", R"(
fn d_fn() -> i32 { return 7; }
)");
  diamondProvider.addModule("std::b", R"(
import std::d::{d_fn};
fn b_fn() -> i32 { return d_fn(); }
)");
  diamondProvider.addModule("std::c", R"(
import std::d::{d_fn};
fn c_fn() -> i32 { return d_fn(); }
)");
  diamondProvider.addModule("std::a", R"(
import std::b::{b_fn};
import std::c::{c_fn};
fn main() -> i32 { return b_fn() + c_fn(); }
)");

  const noria::ResolvedProgram diamondResolved = noria::resolveImports(
      parseModule(R"(
import std::b::{b_fn};
import std::c::{c_fn};
fn main() -> i32 { return b_fn() + c_fn(); }
)"),
      noria::CompileOptions{}, diamondProvider);
  expect(countFunction(diamondResolved.module, "d_fn") == 1,
         "diamond dependency parses d once and merges it once");

  MemoryModuleSourceProvider duplicateProvider;
  duplicateProvider.addModule("std::one", R"(fn dup() -> i32 { return 1; })");
  duplicateProvider.addModule("std::two", R"(fn dup() -> i32 { return 2; })");

  expectThrowsContains(
      [&] {
        noria::resolveImports(parseModule(R"(
import std::one::{dup};
import std::two::{dup};
fn main() -> i32 { return dup(); }
)"),
                              noria::CompileOptions{}, duplicateProvider);
      },
      "duplicate export rejected",
      "import: duplicate symbol 'dup'");

  MemoryModuleSourceProvider orderProvider;
  orderProvider.addModule("std::left", R"(fn left() -> i32 { return 1; })");
  orderProvider.addModule("std::right", R"(fn right() -> i32 { return 2; })");

  const noria::ResolvedProgram firstResolved = noria::resolveImports(
      parseModule(R"(
import std::left::{left};
import std::right::{right};
fn main() -> i32 { return left() + right(); }
)"),
      noria::CompileOptions{}, orderProvider);
  const noria::ResolvedProgram secondResolved = noria::resolveImports(
      parseModule(R"(
import std::left::{left};
import std::right::{right};
fn main() -> i32 { return left() + right(); }
)"),
      noria::CompileOptions{}, orderProvider);

  std::ostringstream firstOrder;
  std::ostringstream secondOrder;
  for (const auto& function : firstResolved.module.functions) {
    firstOrder << function.name << ',';
  }
  for (const auto& function : secondResolved.module.functions) {
    secondOrder << function.name << ',';
  }
  expect(firstOrder.str() == secondOrder.str(), "deterministic merge order across runs");

  MemoryModuleSourceProvider nestedProvider;
  nestedProvider.addModule("std::internal::rt", R"(
fn rt_fn() -> i32 { return 1; }
)");
  nestedProvider.addModule("std::memory", R"(
import std::internal::rt::{rt_fn};
fn memory_fn() -> i32 { return rt_fn(); }
)");

  const noria::ResolvedProgram nestedResolved = noria::resolveImports(
      parseModule(R"(
import std::memory::{memory_fn};
fn main() -> i32 { return memory_fn(); }
)"),
      noria::CompileOptions{}, nestedProvider);
  expect(countFunction(nestedResolved.module, "rt_fn") == 1,
         "nested stdlib path resolves internal module once");

  expectThrowsContains(
      [&] {
        noria::resolveImports(parseModule(R"(
import std::internal::rt::{rt_fn};
fn main() -> i32 { return rt_fn(); }
)"),
                              noria::CompileOptions{}, nestedProvider);
      },
      "internal module import rejected from user code",
      "import: module 'std::internal::rt' is internal and cannot be imported");

  MemoryModuleSourceProvider originProvider;
  originProvider.addModule("std::lib", R"(
fn exported() -> i32 { return 1; }
)");
  const noria::ResolvedProgram originResolved = noria::resolveImports(
      parseModule(R"(
import std::lib::{exported};

struct exported {
  value: i32;
}

fn main() -> i32 { return exported(); }
)"),
      noria::CompileOptions{}, originProvider);
  expect(originResolved.symbolOrigins.functions.at("exported") == "std::lib",
         "imported function keeps stdlib origin");
  expect(originResolved.symbolOrigins.structs.at("exported") == "",
         "root struct keeps empty origin when name matches import");

  MemoryModuleSourceProvider familyProvider;
  familyProvider.addModule("std::impl_family", R"(
fn kind<T, I>(b: Box<T, I>) -> i32 impl arr {
  return 1;
}

fn kind<T, I>(b: Box<T, I>) -> i32 impl list {
  return 2;
}
)");
  const noria::ResolvedProgram familyResolved = noria::resolveImports(
      parseModule(R"(
import std::impl_family::{kind};

struct Box<T, I> {
  value: T;
}

fn main() -> i32 {
  return 0;
}
)"),
      noria::CompileOptions{}, familyProvider);
  expect(countFunction(familyResolved.module, "kind") == 2,
         "importing a name merges every tagged implementation");
  expect(familyResolved.symbolOrigins.functions.at("kind") == "std::impl_family",
         "imported tagged family keeps stdlib origin");

  MemoryModuleSourceProvider intraDupProvider;
  intraDupProvider.addModule("std::dupmod", R"(
fn dup() -> i32 { return 1; }
fn dup() -> i32 { return 2; }
)");
  expectThrowsContains(
      [&] {
        noria::resolveImports(parseModule(R"(
import std::dupmod::{dup};
fn main() -> i32 { return dup(); }
)"),
                              noria::CompileOptions{}, intraDupProvider);
      },
      "intra-module duplicate function rejected",
      "import: duplicate function 'dup'");

  MemoryModuleSourceProvider structDupProvider;
  structDupProvider.addModule("std::pairmod", R"(
struct Pair {
  left: i32;
  right: i32;
}

struct Pair {
  first: i32;
  second: i32;
}
)");
  expectThrowsContains(
      [&] {
        noria::resolveImports(parseModule(R"(
import std::pairmod::{Pair};
fn main() -> i32 { return 0; }
)"),
                              noria::CompileOptions{}, structDupProvider);
      },
      "intra-module duplicate struct rejected",
      "import: duplicate struct 'pair'");

  MemoryModuleSourceProvider conflictProvider;
  conflictProvider.addModule("std::conflict", R"(
struct Box {
  value: i32;
}

fn Box() -> i32 {
  return 1;
}
)");
  expectThrowsContains(
      [&] {
        noria::resolveImports(parseModule(R"(
import std::conflict::{Box};
fn main() -> i32 { return Box(); }
)"),
                              noria::CompileOptions{}, conflictProvider);
      },
      "function and struct export conflict rejected",
      "import: export 'box' is both a function and a struct");

  expectThrowsContains(
      [&] {
        noria::resolveImports(parseModule(R"(
import std::left::{left, left};
fn main() -> i32 { return left(); }
)"),
                              noria::CompileOptions{}, orderProvider);
      },
      "duplicate import name rejected",
      "import: duplicate import 'left'");

  MemoryModuleSourceProvider cachedProvider;
  cachedProvider.addModule("std::cached", R"(
fn cached() -> i32 { return 9; }
)");
  noria::CompilerCache resolverCache;
  noria::CompileOptions cacheOptions;
  cacheOptions.stdlibRoot = std::filesystem::path("/tmp/noria-cache-test-stdlib");

  const noria::ResolvedProgram cachedFirst = noria::resolveImports(
      parseModule(R"(
import std::cached::{cached};
import std::cached::{cached};
fn main() -> i32 { return cached(); }
)"),
      cacheOptions, cachedProvider, &resolverCache);
  const noria::ResolvedProgram cachedSecond = noria::resolveImports(
      parseModule(R"(
import std::cached::{cached};
fn main() -> i32 { return cached(); }
)"),
      cacheOptions, cachedProvider, &resolverCache);

  expect(countFunction(cachedFirst.module, "cached") == 1,
         "repeated same-module imports clone one function");
  expect(countFunction(cachedSecond.module, "cached") == 1,
         "cached module can be reused by a later resolve");
  expect(cachedProvider.loadCount("std::cached") == 1,
         "shared module cache avoids reloading parsed stdlib module");
  expect(resolverCache.parsedStdlibModuleCount() == 1,
         "resolver cache retains parsed stdlib module");

  MemoryModuleSourceProvider containerProvider;
  containerProvider.addModule("std::sequence", R"(
struct Sequence<T, I> {
  value: T;
}
fn sequence_new() -> i32 { return 0; }
fn sequence_get() -> i32 { return 0; }
fn sequence_set() -> i32 { return 0; }
fn sequence_drop() -> i32 { return 0; }
fn sequence_clone() -> i32 { return 0; }
)");
  containerProvider.addModule("std::dictionary", R"(
struct Dictionary<K, V, I> {
  key: K;
  value: V;
}
fn dictionary_new() -> i32 { return 0; }
fn dictionary_get() -> i32 { return 0; }
fn dictionary_contains() -> i32 { return 0; }
fn dictionary_insert() -> i32 { return 0; }
fn dictionary_drop() -> i32 { return 0; }
fn dictionary_clone() -> i32 { return 0; }
)");
  containerProvider.addModule("std::set", R"(
struct Set<T, I> {
  value: T;
}
fn set_new() -> i32 { return 0; }
fn set_contains() -> i32 { return 0; }
fn set_drop() -> i32 { return 0; }
fn set_clone() -> i32 { return 0; }
)");
  noria::ResolvedProgram defaultContainerResolved = noria::resolveImports(
      parseModule(R"(
import std::sequence::{Sequence};
import std::dictionary::{Dictionary};
import std::set::{Set};

fn main(sequence: Sequence<i32>, dictionary: Dictionary<i32, bool>, set: Set<i32>) -> Sequence<i32> {
  return sequence;
}
)"),
      noria::CompileOptions{}, containerProvider);
  noria::applyDefaultAdtImplementations(defaultContainerResolved.module,
                                        defaultContainerResolved.symbolOrigins);
  const noria::ast::Function* defaultContainerFunction =
      findFunction(defaultContainerResolved.module, "main");
  expect(defaultContainerFunction != nullptr, "default container test function is present");
  if (defaultContainerFunction != nullptr) {
    expect(defaultContainerFunction->returnType &&
               hasDefaultImplementation(*defaultContainerFunction->returnType,
                                        noria::ImplementationTag::Arr),
           "Sequence omits to arr through the container registry");
    expect(hasDefaultImplementation(defaultContainerFunction->parameters[0].type,
                                    noria::ImplementationTag::Arr),
           "Sequence parameter default comes from the container registry");
    expect(hasDefaultImplementation(defaultContainerFunction->parameters[1].type,
                                    noria::ImplementationTag::Hashmap),
           "Dictionary parameter default comes from the container registry");
    expect(hasDefaultImplementation(defaultContainerFunction->parameters[2].type,
                                    noria::ImplementationTag::Hashmap),
           "Set parameter default comes from the container registry");
  }

  if (failures != 0) {
    std::cerr << failures << " module resolver test failure(s)\n";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
