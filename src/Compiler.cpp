#include "noria/Compiler.hpp"

#include "noria/Codegen.hpp"
#include "noria/Diagnostic.hpp"
#include "noria/Lexer.hpp"
#include "noria/ModuleResolver.hpp"
#include "noria/Monomorphize.hpp"
#include "noria/Parser.hpp"
#include "noria/TypeChecker.hpp"

namespace noria {

  namespace {

    CompileOutput compileParsedModule(std::vector<Token> tokens, ast::Module module,
                                      StopAfter stopAfter) {
      CompileOutput output;
      output.tokens = std::move(tokens);
      output.module = std::move(module);

      if (stopAfter == StopAfter::Ast) {
        return output;
      }

      TypeChecker checker;
      checker.check(output.module);

      constexpr std::size_t kMaxSpecializationRounds = 8;
      constexpr std::size_t kMaxSpecializations = 64;
      std::size_t totalSpecializations = 0;
      SourceLocation lastSpecializationLocation{};

      for (std::size_t round = 0; round < kMaxSpecializationRounds; ++round) {
        bool expanded = false;

        if (!checker.structSpecializationRequests().empty()) {
          const std::vector<StructSpecializationRequest> structRequests =
              checker.structSpecializationRequests();
          checker.clearStructSpecializationRequests();
          for (const StructSpecializationRequest& request : structRequests) {
            lastSpecializationLocation = request.useSiteLocation;
          }
          totalSpecializations += expandStructSpecializations(output.module, structRequests);
          expanded = true;
        }

        if (!checker.specializationRequests().empty()) {
          const std::vector<SpecializationRequest> requests = checker.specializationRequests();
          checker.clearSpecializationRequests();
          for (const SpecializationRequest& request : requests) {
            lastSpecializationLocation = request.callSiteLocation;
          }
          totalSpecializations += expandSpecializations(output.module, requests);
          expanded = true;
        }

        if (!expanded) {
          break;
        }

        if (totalSpecializations > kMaxSpecializations) {
          throw CompileError(formatDiagnostic(lastSpecializationLocation,
                                              DiagnosticStage::TypeCheck,
                                              "recursive generic specialization"));
        }

        checker.check(output.module);
      }

      if (!checker.specializationRequests().empty() ||
          !checker.structSpecializationRequests().empty()) {
        throw CompileError(formatDiagnostic(lastSpecializationLocation, DiagnosticStage::TypeCheck,
                                            "recursive generic specialization"));
      }

      stripGenericTemplates(output.module);

      if (stopAfter == StopAfter::Typed) {
        return output;
      }

      LlvmIrTextGenerator generator;
      output.llvmIr = generator.generate(output.module);
      return output;
    }

  } // namespace

  CompileOutput compileSource(std::string_view source, StopAfter stopAfter) {
    Lexer lexer(source);
    std::vector<Token> tokens = lexer.lex();
    if (stopAfter == StopAfter::Tokens) {
      CompileOutput output;
      output.tokens = std::move(tokens);
      return output;
    }

    Parser parser(tokens);
    ast::Module module = parser.parseModule();
    if (!module.imports.empty()) {
      throw CompileError("imports require compileSource with CompileOptions");
    }

    return compileParsedModule(std::move(tokens), std::move(module), stopAfter);
  }

  CompileOutput compileSource(std::string_view source, StopAfter stopAfter,
                              const CompileOptions& options) {
    Lexer lexer(source);
    std::vector<Token> tokens = lexer.lex();
    if (stopAfter == StopAfter::Tokens) {
      CompileOutput output;
      output.tokens = std::move(tokens);
      return output;
    }

    Parser parser(tokens);
    ast::Module rootModule = parser.parseModule();

    if (rootModule.imports.empty()) {
      return compileParsedModule(std::move(tokens), std::move(rootModule), stopAfter);
    }

    FileModuleSourceProvider provider(options.stdlibRoot);
    ResolvedProgram resolved = resolveImports(std::move(rootModule), options, provider);
    return compileParsedModule(std::move(tokens), std::move(resolved.module), stopAfter);
  }

} // namespace noria
