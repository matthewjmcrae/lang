#pragma once

#include "noria/Monomorphize.hpp"

#include <unordered_map>
#include <unordered_set>

namespace noria {

  class CompilerCache;
  struct SymbolOrigins;

  namespace monomorphize_detail {

    struct SpecializationRegistry {
      void seedFromModule(const ast::Module& module);
      bool hasFunction(std::string_view mangledName) const;
      bool hasStruct(std::string_view mangledName) const;

      std::unordered_set<std::string> emittedFunctions;
      std::unordered_set<std::string> emittedStructs;
      std::unordered_map<std::string, std::vector<Type>> functionTypeArgs;
    };

    struct SpecializationGraph {
      void link(std::string_view childMangled, std::string_view parentMangled,
                SourceLocation location);
      void clear() { dependencyParent.clear(); }

      std::unordered_map<std::string, std::string> dependencyParent;

    private:
      [[noreturn]] void throwCycle(SourceLocation location, std::string_view childMangled,
                                   std::string_view parentMangled) const;
    };

    struct SpecializationEmitter {
      SpecializationEmitter(CompilerCache* compilerCache, SymbolOrigins* symbolOrigins)
          : compilerCache(compilerCache), symbolOrigins(symbolOrigins) {}

      std::size_t emitFunction(ast::Module& module, const SpecializationRequest& request,
                               SpecializationRegistry& registry);
      std::size_t emitStruct(ast::Module& module, const StructSpecializationRequest& request,
                             SpecializationRegistry& registry);

      CompilerCache* compilerCache = nullptr;
      SymbolOrigins* symbolOrigins = nullptr;
    };

  } // namespace monomorphize_detail

  class SpecializationCache::Impl {
  public:
    Impl(CompilerCache* compilerCache, SymbolOrigins* symbolOrigins)
        : emitter(compilerCache, symbolOrigins) {}

    monomorphize_detail::SpecializationRegistry registry;
    monomorphize_detail::SpecializationGraph graph;
    monomorphize_detail::SpecializationEmitter emitter;
  };

} // namespace noria
