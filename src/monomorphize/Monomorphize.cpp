#include "MonomorphizeInternal.hpp"

#include "../internal/AstVisitorAdapters.hpp"
#include "../internal/SpecializationSupport.hpp"
#include "noria/AstClone.hpp"
#include "noria/AstVisitor.hpp"
#include "noria/CompilerCache.hpp"
#include "noria/Diagnostic.hpp"
#include "noria/ModuleResolver.hpp"
#include "noria/SemanticTables.hpp"
#include "noria/TypeChecker.hpp"

#include <algorithm>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace noria::monomorphize_detail {

  using internal::containsUnboundTypeParam;
  using internal::findImplTag;

  bool locationLess(const SourceLocation& lhs, const SourceLocation& rhs) {
    if (lhs.file != rhs.file) {
      if (lhs.file.empty()) {
        return !rhs.file.empty();
      }
      if (rhs.file.empty()) {
        return false;
      }
      return lhs.file < rhs.file;
    }
    if (lhs.line != rhs.line) {
      return lhs.line < rhs.line;
    }
    return lhs.column < rhs.column;
  }

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

  PendingSpecializations takePendingSpecializations(TypeChecker& checker) {
    return PendingSpecializations{checker.takeStructSpecializationRequests(),
                                  checker.takeSpecializationRequests()};
  }

  void appendPendingSpecializations(PendingSpecializations& accumulated,
                                    const PendingSpecializations& pending) {
    accumulated.structs.insert(accumulated.structs.end(), pending.structs.begin(),
                               pending.structs.end());
    accumulated.functions.insert(accumulated.functions.end(), pending.functions.begin(),
                                 pending.functions.end());
  }

  void linkNewSpecializations(SpecializationCache& cache,
                              const std::vector<StructSpecializationRequest>& structRequests,
                              const std::vector<SpecializationRequest>& functionRequests) {
    std::unordered_set<std::string> linked;
    for (const StructSpecializationRequest& request : structRequests) {
      const std::string childMangled = mangleSpecialization(request.templateName, request.typeArgs);
      if (!linked.insert(childMangled).second) {
        continue;
      }
      cache.link(childMangled, parentSpecializationMangled(request.enclosingFunction),
                 request.useSiteLocation);
    }
    for (const SpecializationRequest& request : functionRequests) {
      const std::string childMangled = mangleSpecialization(request.templateName, request.typeArgs);
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

  void ensureExpansionLimit(std::size_t totalSpecializations,
                            SourceLocation lastSpecializationLocation) {
    static constexpr std::size_t kMaxSpecializations = 64;
    if (totalSpecializations > kMaxSpecializations) {
      throwExpansionLimit(lastSpecializationLocation);
    }
  }

  std::vector<SpecializationRequest>
  sortedFunctionRequests(const std::vector<SpecializationRequest>& requests) {
    std::vector<SpecializationRequest> sorted = requests;
    std::sort(sorted.begin(), sorted.end(),
              [](const SpecializationRequest& left, const SpecializationRequest& right) {
                const std::string leftName = mangleSpecialization(left.templateName, left.typeArgs);
                const std::string rightName =
                    mangleSpecialization(right.templateName, right.typeArgs);
                if (leftName != rightName) {
                  return leftName < rightName;
                }
                if (left.templateName != right.templateName) {
                  return left.templateName < right.templateName;
                }
                return locationLess(left.callSiteLocation, right.callSiteLocation);
              });
    return sorted;
  }

  std::vector<StructSpecializationRequest>
  sortedStructRequests(const std::vector<StructSpecializationRequest>& requests) {
    std::vector<StructSpecializationRequest> sorted = requests;
    std::sort(
        sorted.begin(), sorted.end(),
        [](const StructSpecializationRequest& left, const StructSpecializationRequest& right) {
          const std::string leftName = mangleSpecialization(left.templateName, left.typeArgs);
          const std::string rightName = mangleSpecialization(right.templateName, right.typeArgs);
          if (leftName != rightName) {
            return leftName < rightName;
          }
          if (left.templateName != right.templateName) {
            return left.templateName < right.templateName;
          }
          return locationLess(left.useSiteLocation, right.useSiteLocation);
        });
    return sorted;
  }

  std::size_t
  emitUniqueFunctionSpecializations(ast::Module& module,
                                    const std::vector<SpecializationRequest>& sortedRequests,
                                    SpecializationCache& cache) {
    std::unordered_set<std::string> seen;
    std::size_t added = 0;

    module.functions.reserve(module.functions.size() + sortedRequests.size());
    for (const SpecializationRequest& request : sortedRequests) {
      const std::string mangledName = mangleSpecialization(request.templateName, request.typeArgs);
      if (!seen.insert(mangledName).second) {
        continue;
      }

      added += cache.emitFunction(module, request);
    }
    return added;
  }

  std::size_t
  emitUniqueStructSpecializations(ast::Module& module,
                                  const std::vector<StructSpecializationRequest>& sortedRequests,
                                  SpecializationCache& cache) {
    std::unordered_set<std::string> seen;
    std::size_t added = 0;

    module.structs.reserve(module.structs.size() + sortedRequests.size());
    for (const StructSpecializationRequest& request : sortedRequests) {
      const std::string mangledName = mangleSpecialization(request.templateName, request.typeArgs);
      if (!seen.insert(mangledName).second) {
        continue;
      }

      added += cache.emitStruct(module, request);
    }
    return added;
  }

  std::size_t emitPendingSpecializations(ast::Module& module, SpecializationCache& cache,
                                         const PendingSpecializations& pending) {
    std::size_t added = 0;
    if (!pending.structs.empty()) {
      const std::vector<StructSpecializationRequest> sorted = sortedStructRequests(pending.structs);
      added += emitUniqueStructSpecializations(module, sorted, cache);
    }
    if (!pending.functions.empty()) {
      const std::vector<SpecializationRequest> sorted = sortedFunctionRequests(pending.functions);
      added += emitUniqueFunctionSpecializations(module, sorted, cache);
    }
    return added;
  }

  void preparePendingSpecializations(TypeChecker& checker, SymbolOrigins& symbolOrigins,
                                     const PendingSpecializations& pending,
                                     SourceLocation& lastSpecializationLocation) {
    for (const StructSpecializationRequest& request : pending.structs) {
      lastSpecializationLocation = request.useSiteLocation;
      propagateStructSpecializationOrigin(symbolOrigins, request.templateName, request.typeArgs);
      checker.registerStructSpecialization(
          mangleSpecialization(request.templateName, request.typeArgs), request.typeArgs);
    }
    for (const SpecializationRequest& request : pending.functions) {
      lastSpecializationLocation = request.callSiteLocation;
      propagateFunctionSpecializationOrigin(symbolOrigins, request.templateName, request.typeArgs);
      checker.registerFunctionSpecialization(
          mangleSpecialization(request.templateName, request.typeArgs), request.typeArgs);
    }
  }

  void rewriteFinalSpecializations(ast::Module& module, const PendingSpecializations& accumulated) {
    if (!accumulated.functions.empty()) {
      rewriteTargetedGenericCallSites(module, accumulated.functions);
    }
    if (!accumulated.structs.empty()) {
      rewriteStructApplications(module, accumulated.structs);
    }
  }

} // namespace noria::monomorphize_detail

namespace noria {

  using namespace monomorphize_detail;

  std::size_t expandSpecializations(ast::Module& module,
                                    const std::vector<SpecializationRequest>& requests,
                                    SpecializationCache& cache) {
    // Emit deterministic, deduplicated function clones before rewriting call sites to them.
    const std::vector<SpecializationRequest> sorted = sortedFunctionRequests(requests);
    const std::size_t added = emitUniqueFunctionSpecializations(module, sorted, cache);
    rewriteGenericCallSites(module, requests);
    return added;
  }

  std::size_t expandStructSpecializations(ast::Module& module,
                                          const std::vector<StructSpecializationRequest>& requests,
                                          SpecializationCache& cache) {
    // Emit deterministic, deduplicated struct declarations before normalizing applied types.
    const std::vector<StructSpecializationRequest> sorted = sortedStructRequests(requests);
    const std::size_t added = emitUniqueStructSpecializations(module, sorted, cache);
    rewriteStructApplications(module, requests);

    return added;
  }

  void stripGenericTemplates(ast::Module& module) {
    module.structs.erase(std::remove_if(module.structs.begin(), module.structs.end(),
                                        [](const ast::StructDecl& structDecl) {
                                          return !structDecl.typeParams.empty();
                                        }),
                         module.structs.end());
  }

  MonomorphizationResult monomorphizeGenerics(ast::Module& module, TypeChecker& checker,
                                              SymbolOrigins& symbolOrigins,
                                              CompilerCache* compilerCache) {
    static constexpr std::size_t kMaxSpecializationRounds = 64;
    SpecializationCache cache(compilerCache, &symbolOrigins);
    PendingSpecializations accumulated;
    PendingSpecializations pending = takePendingSpecializations(checker);
    std::size_t totalSpecializations = 0;
    SourceLocation lastSpecializationLocation{};

    cache.seedFromModule(module);
    for (std::size_t round = 0; round < kMaxSpecializationRounds; ++round) {
      if (pending.empty()) {
        break;
      }

      // Link the current frontier before emission so recursive generic cycles are caught.
      linkNewSpecializations(cache, pending.structs, pending.functions);
      const std::size_t firstNewStruct = module.structs.size();
      const std::size_t firstNewFunction = module.functions.size();

      preparePendingSpecializations(checker, symbolOrigins, pending, lastSpecializationLocation);
      appendPendingSpecializations(accumulated, pending);
      totalSpecializations += emitPendingSpecializations(module, cache, pending);
      ensureExpansionLimit(totalSpecializations, lastSpecializationLocation);

      checker.checkSpecializationFrontier(module, firstNewStruct, firstNewFunction, symbolOrigins);
      pending = takePendingSpecializations(checker);
    }
    cache.clearLinks();

    if (!pending.empty()) {
      throwExpansionLimit(lastSpecializationLocation);
    }

    rewriteFinalSpecializations(module, accumulated);
    stripGenericTemplates(module);
    return MonomorphizationResult{cache.functionSpecializationTypeArgs(),
                                  cache.structSpecializationTypeArgs()};
  }

} // namespace noria
