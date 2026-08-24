#include "noria/Compiler.hpp"

#include "noria/Codegen.hpp"
#include "noria/CompilerCache.hpp"
#include "noria/Diagnostic.hpp"
#include "noria/Lexer.hpp"
#include "noria/ModuleResolver.hpp"
#include "noria/Monomorphize.hpp"
#include "noria/Parser.hpp"
#include "noria/TypeChecker.hpp"

#include <filesystem>

namespace noria {

  namespace {

    PipelineOutput compileParsedModule(std::vector<Token> tokens, ast::Module module,
                                       StopAfter stopAfter, SymbolOrigins symbolOrigins = {}) {
      PipelineOutput output;
      output.tokens = std::move(tokens);
      output.module = std::move(module);

      if (stopAfter == StopAfter::Ast) {
        return output;
      }

      TypeChecker checker;
      checker.check(output.module, symbolOrigins);

      const MonomorphizationResult monomorphization =
          monomorphizeGenerics(output.module, checker, symbolOrigins, &processCompilerCache());

      if (stopAfter == StopAfter::Typed) {
        return output;
      }

      LLVMGenerator generator;
      generator.setFunctionSpecializationTypeArgs(monomorphization.functionSpecializationTypeArgs);
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
