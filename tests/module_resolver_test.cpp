#include "noria/Compiler.hpp"
#include "noria/Diagnostic.hpp"
#include "noria/Lexer.hpp"
#include "noria/ModuleResolver.hpp"
#include "noria/Parser.hpp"

#include <cstdlib>
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
      const auto iterator = sources_.find(modulePath);
      if (iterator == sources_.end()) {
        return std::nullopt;
      }
      return iterator->second;
    }

  private:
    std::unordered_map<std::string, std::string> sources_;
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

  if (failures != 0) {
    std::cerr << failures << " module resolver test failure(s)\n";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
