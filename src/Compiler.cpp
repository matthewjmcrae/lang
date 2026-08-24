#include "noria/Compiler.hpp"

#include "noria/Codegen.hpp"
#include "noria/Diagnostic.hpp"
#include "noria/Lexer.hpp"
#include "noria/ModuleResolver.hpp"
#include "noria/Monomorphize.hpp"
#include "noria/Parser.hpp"
#include "noria/TypeChecker.hpp"

#include <unordered_set>

namespace noria {

  namespace {

    void propagateFunctionSpecializationOrigin(SymbolOrigins& symbolOrigins,
                                               std::string_view templateName,
                                               const std::vector<Type>& typeArgs) {
      const auto templateOrigin = symbolOrigins.functions.find(std::string(templateName));
      if (templateOrigin == symbolOrigins.functions.end()) {
        return;
      }
      symbolOrigins.functions.emplace(mangleSpecialization(templateName, typeArgs),
                                      templateOrigin->second);
    }

    void propagateStructSpecializationOrigin(SymbolOrigins& symbolOrigins,
                                             std::string_view templateName,
                                             const std::vector<Type>& typeArgs) {
      const auto templateOrigin = symbolOrigins.structs.find(std::string(templateName));
      if (templateOrigin == symbolOrigins.structs.end()) {
        return;
      }
      symbolOrigins.structs.emplace(mangleSpecialization(templateName, typeArgs),
                                    templateOrigin->second);
    }

    std::string parentSpecializationMangled(std::string_view enclosingFunction) {
      if (enclosingFunction.find('$') == std::string_view::npos) {
        return {};
      }
      return std::string(enclosingFunction);
    }

    void linkNewSpecializations(SpecializationCache& cache,
                                const std::vector<StructSpecializationRequest>& structRequests,
                                const std::vector<SpecializationRequest>& functionRequests) {
      std::unordered_set<std::string> linked;
      for (const StructSpecializationRequest& request : structRequests) {
        const std::string childMangled =
            mangleSpecialization(request.templateName, request.typeArgs);
        if (!linked.insert(childMangled).second) {
          continue;
        }
        cache.link(childMangled, parentSpecializationMangled(request.enclosingFunction),
                   request.useSiteLocation);
      }
      for (const SpecializationRequest& request : functionRequests) {
        const std::string childMangled =
            mangleSpecialization(request.templateName, request.typeArgs);
        if (!linked.insert(childMangled).second) {
          continue;
        }
        cache.link(childMangled, parentSpecializationMangled(request.enclosingFunction),
                   request.callSiteLocation);
      }
    }

    [[noreturn]] void throwExpansionLimit(SourceLocation location) {
      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                          "specialization expansion limit exceeded"));
    }

    CompileOutput compileParsedModule(std::vector<Token> tokens, ast::Module module,
                                      StopAfter stopAfter, SymbolOrigins symbolOrigins = {}) {
      CompileOutput output;
      output.tokens = std::move(tokens);
      output.module = std::move(module);

      if (stopAfter == StopAfter::Ast) {
        return output;
      }

      TypeChecker checker;
      checker.check(output.module, symbolOrigins);

      SpecializationCache cache;
      cache.seedFromModule(output.module);

      constexpr std::size_t kMaxSpecializationRounds = 64;
      constexpr std::size_t kMaxSpecializations = 64;
      std::size_t totalSpecializations = 0;
      SourceLocation lastSpecializationLocation{};

      for (std::size_t round = 0; round < kMaxSpecializationRounds; ++round) {
        bool expanded = false;

        const std::vector<StructSpecializationRequest> structRequests =
            checker.structSpecializationRequests();
        const std::vector<SpecializationRequest> functionRequests =
            checker.specializationRequests();

        if (!structRequests.empty() || !functionRequests.empty()) {
          linkNewSpecializations(cache, structRequests, functionRequests);
        }

        if (!structRequests.empty()) {
          checker.clearStructSpecializationRequests();
          for (const StructSpecializationRequest& request : structRequests) {
            lastSpecializationLocation = request.useSiteLocation;
            propagateStructSpecializationOrigin(symbolOrigins, request.templateName,
                                                request.typeArgs);
          }
          totalSpecializations += expandStructSpecializations(output.module, structRequests, cache);
          expanded = true;
        }

        if (!functionRequests.empty()) {
          checker.clearSpecializationRequests();
          for (const SpecializationRequest& request : functionRequests) {
            lastSpecializationLocation = request.callSiteLocation;
            propagateFunctionSpecializationOrigin(symbolOrigins, request.templateName,
                                                  request.typeArgs);
            checker.registerFunctionSpecialization(
                mangleSpecialization(request.templateName, request.typeArgs), request.typeArgs);
          }
          totalSpecializations += expandSpecializations(output.module, functionRequests, cache);
          expanded = true;
        }

        if (!expanded) {
          cache.clearLinks();
          break;
        }

        if (totalSpecializations > kMaxSpecializations) {
          throwExpansionLimit(lastSpecializationLocation);
        }

        checker.check(output.module, symbolOrigins);
      }

      if (!checker.specializationRequests().empty() ||
          !checker.structSpecializationRequests().empty()) {
        throwExpansionLimit(lastSpecializationLocation);
      }

      stripGenericTemplates(output.module);

      if (stopAfter == StopAfter::Typed) {
        return output;
      }

      LlvmIrTextGenerator generator;
      generator.setFunctionSpecializationTypeArgs(cache.functionSpecializationTypeArgs());
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
    Lexer lexer(source, options.rootFileName);
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
    return compileParsedModule(std::move(tokens), std::move(resolved.module), stopAfter,
                               resolved.symbolOrigins);
  }

} // namespace noria
