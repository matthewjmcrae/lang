#include "MonomorphizeInternal.hpp"
#include "SpecializationCacheInternal.hpp"

#include "../internal/SpecializationSupport.hpp"
#include "noria/CompilerCache.hpp"
#include "noria/Diagnostic.hpp"
#include "noria/ModuleResolver.hpp"

#include <memory>
#include <optional>
#include <sstream>
#include <utility>

namespace noria::monomorphize_detail {

  namespace {

    bool isStdlibOrigin(std::string_view modulePath) {
      return modulePath.rfind("std::", 0) == 0;
    }

    std::optional<std::string>
    stdlibFunctionSpecializationCacheKey(const SymbolOrigins* symbolOrigins,
                                         const SpecializationRequest& request,
                                         const std::string& mangledName) {
      if (symbolOrigins == nullptr) {
        return std::nullopt;
      }
      const auto origin = symbolOrigins->functions.find(request.templateName);
      if (origin == symbolOrigins->functions.end() || !isStdlibOrigin(origin->second)) {
        return std::nullopt;
      }
      return stdlibSpecializationCacheKey("fn", origin->second, mangledName);
    }

    std::optional<std::string>
    stdlibStructSpecializationCacheKey(const SymbolOrigins* symbolOrigins,
                                       const StructSpecializationRequest& request,
                                       const std::string& mangledName) {
      if (symbolOrigins == nullptr) {
        return std::nullopt;
      }
      const auto origin = symbolOrigins->structs.find(request.templateName);
      if (origin == symbolOrigins->structs.end() || !isStdlibOrigin(origin->second)) {
        return std::nullopt;
      }
      return stdlibSpecializationCacheKey("struct", origin->second, mangledName);
    }

  } // namespace

  void SpecializationRegistry::seedFromModule(const ast::Module& module) {
    for (const auto& function : module.functions) {
      if (function.typeParams.empty()) {
        emittedFunctions.insert(function.name);
      }
    }
    for (const auto& structDecl : module.structs) {
      if (structDecl.typeParams.empty()) {
        emittedStructs.insert(structDecl.name);
      }
    }
  }

  bool SpecializationRegistry::hasFunction(std::string_view mangledName) const {
    return emittedFunctions.contains(std::string(mangledName));
  }

  bool SpecializationRegistry::hasStruct(std::string_view mangledName) const {
    return emittedStructs.contains(std::string(mangledName));
  }

  void SpecializationGraph::throwCycle(SourceLocation location, std::string_view childMangled,
                                       std::string_view parentMangled) const {
    std::ostringstream out;
    out << "recursive generic specialization: " << childMangled;
    std::string current(parentMangled);
    while (current != childMangled) {
      out << " -> " << current;
      const auto parent = dependencyParent.find(current);
      if (parent == dependencyParent.end()) {
        break;
      }
      current = parent->second;
    }
    out << " -> " << childMangled;
    throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck, out.str()));
  }

  void SpecializationGraph::link(std::string_view childMangled, std::string_view parentMangled,
                                 SourceLocation location) {
    const std::string child(childMangled);
    std::string current(parentMangled);
    while (true) {
      if (current == child) {
        throwCycle(location, child, parentMangled);
      }
      const auto parent = dependencyParent.find(current);
      if (parent == dependencyParent.end()) {
        break;
      }
      current = parent->second;
    }
    dependencyParent.emplace(child, std::string(parentMangled));
  }

  std::size_t SpecializationEmitter::emitFunction(ast::Module& module,
                                                  const SpecializationRequest& request,
                                                  SpecializationRegistry& registry) {
    const std::string mangledName = mangleSpecialization(request.templateName, request.typeArgs);
    if (registry.hasFunction(mangledName)) {
      return 0;
    }

    const std::optional<std::string> cacheKey =
        stdlibFunctionSpecializationCacheKey(symbolOrigins, request, mangledName);
    if (compilerCache != nullptr && cacheKey.has_value()) {
      if (std::optional<CachedFunctionSpecialization> cached =
              compilerCache->cloneStdlibFunctionSpecialization(*cacheKey)) {
        module.functions.push_back(std::move(cached->function));
        registry.emittedFunctions.insert(mangledName);
        registry.functionTypeArgs.emplace(mangledName, std::move(cached->typeArgs));
        return 1;
      }
    }

    const ast::Function* templated =
        findTemplateFunction(module, request.templateName, internal::findImplTag(request.typeArgs));
    if (templated == nullptr) {
      throw CompileError(
          formatDiagnostic(request.callSiteLocation, DiagnosticStage::TypeCheck,
                           "unknown generic function '" + request.templateName + "'"));
    }

    ast::Function specialized = cloneSpecialization(*templated, request.typeArgs);
    if (compilerCache != nullptr && cacheKey.has_value()) {
      compilerCache->storeStdlibFunctionSpecialization(*cacheKey, specialized, request.typeArgs);
    }
    module.functions.push_back(std::move(specialized));
    registry.emittedFunctions.insert(mangledName);
    registry.functionTypeArgs.emplace(mangledName, request.typeArgs);
    return 1;
  }

  std::size_t SpecializationEmitter::emitStruct(ast::Module& module,
                                                const StructSpecializationRequest& request,
                                                SpecializationRegistry& registry) {
    const std::string mangledName = mangleSpecialization(request.templateName, request.typeArgs);
    if (registry.hasStruct(mangledName)) {
      return 0;
    }

    const std::optional<std::string> cacheKey =
        stdlibStructSpecializationCacheKey(symbolOrigins, request, mangledName);
    if (compilerCache != nullptr && cacheKey.has_value()) {
      if (std::optional<ast::StructDecl> cached =
              compilerCache->cloneStdlibStructSpecialization(*cacheKey)) {
        module.structs.push_back(std::move(*cached));
        registry.emittedStructs.insert(mangledName);
        return 1;
      }
    }

    const ast::StructDecl* templated = findTemplateStruct(module, request.templateName);
    if (templated == nullptr) {
      throw CompileError(formatDiagnostic(request.useSiteLocation, DiagnosticStage::TypeCheck,
                                          "unknown generic struct '" + request.templateName + "'"));
    }

    ast::StructDecl specialized = cloneStructSpecialization(*templated, request.typeArgs);
    if (compilerCache != nullptr && cacheKey.has_value()) {
      compilerCache->storeStdlibStructSpecialization(*cacheKey, specialized);
    }
    module.structs.push_back(std::move(specialized));
    registry.emittedStructs.insert(mangledName);
    return 1;
  }

} // namespace noria::monomorphize_detail

namespace noria {

  SpecializationCache::SpecializationCache(CompilerCache* compilerCache,
                                           SymbolOrigins* symbolOrigins)
      : impl_(std::make_unique<Impl>(compilerCache, symbolOrigins)) {}
  SpecializationCache::~SpecializationCache() = default;
  SpecializationCache::SpecializationCache(const SpecializationCache& other)
      : impl_(std::make_unique<Impl>(*other.impl_)) {}
  SpecializationCache& SpecializationCache::operator=(const SpecializationCache& other) {
    if (this != &other) {
      impl_ = std::make_unique<Impl>(*other.impl_);
    }
    return *this;
  }
  SpecializationCache::SpecializationCache(SpecializationCache&& other) noexcept = default;
  SpecializationCache&
  SpecializationCache::operator=(SpecializationCache&& other) noexcept = default;

  void SpecializationCache::seedFromModule(const ast::Module& module) {
    impl_->registry.seedFromModule(module);
  }
  bool SpecializationCache::hasFunction(std::string_view mangledName) const {
    return impl_->registry.hasFunction(mangledName);
  }
  bool SpecializationCache::hasStruct(std::string_view mangledName) const {
    return impl_->registry.hasStruct(mangledName);
  }
  void SpecializationCache::link(std::string_view childMangled, std::string_view parentMangled,
                                 SourceLocation location) {
    if (hasFunction(childMangled) || hasStruct(childMangled) || parentMangled.empty()) {
      return;
    }
    impl_->graph.link(childMangled, parentMangled, location);
  }
  void SpecializationCache::clearLinks() {
    impl_->graph.clear();
  }
  std::size_t SpecializationCache::emitFunction(ast::Module& module,
                                                const SpecializationRequest& request) {
    return impl_->emitter.emitFunction(module, request, impl_->registry);
  }
  std::size_t SpecializationCache::emitStruct(ast::Module& module,
                                              const StructSpecializationRequest& request) {
    return impl_->emitter.emitStruct(module, request, impl_->registry);
  }
  const std::unordered_map<std::string, std::vector<Type>>&
  SpecializationCache::functionSpecializationTypeArgs() const {
    return impl_->registry.functionTypeArgs;
  }

} // namespace noria
