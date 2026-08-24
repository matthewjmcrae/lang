#pragma once

#include "noria/Ast.hpp"
#include "noria/LfuCache.hpp"

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noria {

  struct CachedFunctionSpecialization {
    ast::Function function;
    std::vector<Type> typeArgs;
  };

  class CompilerCache {
  public:
    static constexpr std::size_t kMaxParsedStdlibModules = 64;
    static constexpr std::size_t kMaxParsedStdlibSourceBytes = 8 * 1024 * 1024;
    static constexpr std::size_t kMaxStdlibSpecializations = 256;
    static constexpr std::size_t kMaxStdlibSpecializationWeight = 32 * 1024 * 1024;
    static constexpr std::size_t kMinCachedStdlibFunctionSpecializationWeight = 1024;
    static constexpr std::size_t kMinCachedStdlibStructFields = 8;

    CompilerCache();

    std::optional<ast::Module> cloneParsedStdlibModule(const std::string& key);
    void storeParsedStdlibModule(const std::string& key, const ast::Module& module,
                                 std::size_t sourceBytes);

    std::optional<CachedFunctionSpecialization>
    cloneStdlibFunctionSpecialization(const std::string& key);
    void storeStdlibFunctionSpecialization(const std::string& key,
                                           const ast::Function& function,
                                           const std::vector<Type>& typeArgs);

    std::optional<ast::StructDecl> cloneStdlibStructSpecialization(const std::string& key);
    void storeStdlibStructSpecialization(const std::string& key,
                                         const ast::StructDecl& structDecl);

    std::size_t parsedStdlibModuleCount() const;
    std::size_t stdlibSpecializationCount() const;
    void clear();

  private:
    struct CachedParsedModule {
      ast::Module module;
    };

    struct CachedSpecialization {
      enum class Kind { Function, Struct };

      Kind kind = Kind::Function;
      ast::Function function;
      ast::StructDecl structDecl;
      std::vector<Type> functionTypeArgs;
    };

    mutable std::mutex mutex_;
    LfuCache<std::string, CachedParsedModule> parsedStdlibModules_;
    LfuCache<std::string, CachedSpecialization> stdlibSpecializations_;
  };

  CompilerCache& processCompilerCache();
  std::string stdlibRootCacheKey(const std::filesystem::path& stdlibRoot);
  std::string parsedStdlibModuleCacheKey(const std::string& stdlibRootKey,
                                         const std::string& modulePath);
  std::string stdlibSpecializationCacheKey(std::string_view kind,
                                           const std::string& originModule,
                                           const std::string& mangledName);

} // namespace noria
