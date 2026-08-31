#include "noria/Compiler.hpp"

#include "noria/Codegen.hpp"
#include "noria/CompilerCache.hpp"
#include "noria/Diagnostic.hpp"
#include "noria/Lexer.hpp"
#include "noria/ModuleResolver.hpp"
#include "noria/Monomorphize.hpp"
#include "noria/Parser.hpp"
#include "noria/TypeChecker.hpp"

#include <algorithm>
#include <filesystem>

namespace noria {

  namespace {

    void validateMainEntryPoint(const ast::Module& module, SourceLocation endOfFile) {
      const auto main =
          std::find_if(module.functions.begin(), module.functions.end(),
                       [](const ast::Function& function) { return function.name == "main"; });
      if (main == module.functions.end()) {
        throw CompileError(formatDiagnostic(endOfFile, DiagnosticStage::TypeCheck,
                                            "missing entry point; expected 'fn main() -> i32'"));
      }

      if (!main->typeParams.empty() || main->implTag.has_value()) {
        throw CompileError(formatDiagnostic(
            main->location, DiagnosticStage::TypeCheck,
            "entry point 'main' must not be generic; expected 'fn main() -> i32'"));
      }
      if (!main->parameters.empty()) {
        throw CompileError(formatDiagnostic(
            main->location, DiagnosticStage::TypeCheck,
            "entry point 'main' must accept no parameters; expected 'fn main() -> i32'"));
      }
      if (!main->returnType || *main->returnType != Type::i32()) {
        throw CompileError(
            formatDiagnostic(main->location, DiagnosticStage::TypeCheck,
                             "entry point 'main' must return i32, got " +
                                 (main->returnType ? main->returnType->name()
                                                   : std::string{"<inferred>"})));
      }
    }

    PipelineOutput compileParsedModule(std::vector<Token> tokens, ast::Module module,
                                       StopAfter stopAfter, SymbolOrigins symbolOrigins = {}) {
      PipelineOutput output;
      applyDefaultAdtImplementations(module, symbolOrigins);
      output.tokens = std::move(tokens);
      output.module = std::move(module);

      if (stopAfter == StopAfter::Ast) {
        return output;
      }

      TypeChecker checker;
      checker.check(output.module, symbolOrigins);
      validateMainEntryPoint(output.module, output.tokens.back().location);

      const MonomorphizationResult monomorphization =
          monomorphizeGenerics(output.module, checker, symbolOrigins, &processCompilerCache());

      if (stopAfter == StopAfter::Typed) {
        return output;
      }

      LLVMGenerator generator;
      generator.setFunctionSpecializationTypeArgs(monomorphization.functionSpecializationTypeArgs);
      generator.setStructSpecializationTypeArgs(monomorphization.structSpecializationTypeArgs);
      output.LLVM = generator.generate(output.module);
      return output;
    }

  } // namespace

  PipelineOutput compileSource(std::string_view source, StopAfter stopAfter,
                               const CompileOptions& options) {

    Lexer lexer(source, options.rootFileName);
    std::vector<Token> tokens = lexer.lex();
    if (stopAfter == StopAfter::Tokens) {
      PipelineOutput output;
      output.tokens = std::move(tokens);
      return output;
    }

    Parser parser(tokens);
    ast::Module rootModule = parser.parseModule();

    if (rootModule.imports.empty()) {
      return compileParsedModule(std::move(tokens), std::move(rootModule), stopAfter);
    }

    if (!options.stdlibRoot.has_value()) {
      throw CompileError("imports require compileSource with CompileOptions");
    }
    if (!std::filesystem::is_directory(*options.stdlibRoot)) {
      throw CompileError("stdlib root does not exist: " + options.stdlibRoot->string());
    }

    FileModuleSourceProvider provider(*options.stdlibRoot);
    ResolvedProgram resolved =
        resolveImports(std::move(rootModule), options, provider, &processCompilerCache());
    return compileParsedModule(std::move(tokens), std::move(resolved.module), stopAfter,
                               resolved.symbolOrigins);
  }

} // namespace noria
