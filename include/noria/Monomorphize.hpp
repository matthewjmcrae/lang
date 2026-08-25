#pragma once

#include "noria/Ast.hpp"
#include "noria/Types.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace noria {

  class TypeChecker;
  class CompilerCache;
  struct SymbolOrigins;

  using Substitution = std::unordered_map<std::string, Type>;

  struct SpecializationRequest {
    std::string templateName;
    std::vector<Type> typeArgs;
    SourceLocation callSiteLocation;
    std::string enclosingFunction;
  };

  struct StructSpecializationRequest {
    std::string templateName;
    std::vector<Type> typeArgs;
    SourceLocation useSiteLocation;
    std::string enclosingFunction;
  };

  class SpecializationCache {
  public:
    explicit SpecializationCache(CompilerCache* compilerCache = nullptr,
                                 SymbolOrigins* symbolOrigins = nullptr);
    ~SpecializationCache();
    SpecializationCache(const SpecializationCache& other);
    SpecializationCache& operator=(const SpecializationCache& other);
    SpecializationCache(SpecializationCache&& other) noexcept;
    SpecializationCache& operator=(SpecializationCache&& other) noexcept;

    void seedFromModule(const ast::Module& module);

    bool hasFunction(std::string_view mangledName) const;
    bool hasStruct(std::string_view mangledName) const;

    void link(std::string_view childMangled, std::string_view parentMangled,
              SourceLocation location);
    void clearLinks();

    std::size_t emitFunction(ast::Module& module, const SpecializationRequest& request);
    std::size_t emitStruct(ast::Module& module, const StructSpecializationRequest& request);

    const std::unordered_map<std::string, std::vector<Type>>&
    functionSpecializationTypeArgs() const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
  };

  struct MonomorphizationResult {
    std::unordered_map<std::string, std::vector<Type>> functionSpecializationTypeArgs;
  };

  Type substitute(const Type& type, const Substitution& substitution);

  Type substituteSpecializationType(const Type& type, const Substitution& substitution);

  std::string mangleType(const Type& type);
  std::string mangleSpecialization(std::string_view templateName,
                                   const std::vector<Type>& typeArgs);

  std::size_t expandStructSpecializations(ast::Module& module,
                                          const std::vector<StructSpecializationRequest>& requests,
                                          SpecializationCache& cache);

  std::size_t expandSpecializations(ast::Module& module,
                                    const std::vector<SpecializationRequest>& requests,
                                    SpecializationCache& cache);

  void rewriteGenericCallSites(ast::Module& module,
                               const std::vector<SpecializationRequest>& requests);

  MonomorphizationResult monomorphizeGenerics(ast::Module& module, TypeChecker& checker,
                                              SymbolOrigins& symbolOrigins,
                                              CompilerCache* compilerCache = nullptr);

  void stripGenericTemplates(ast::Module& module);

} // namespace noria
