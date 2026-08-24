#pragma once

#include "noria/Ast.hpp"
#include "noria/Compiler.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace noria {

  class ModuleSourceProvider {
  public:
    virtual ~ModuleSourceProvider() = default;

    virtual std::optional<std::string> loadModuleSource(const std::string& modulePath) = 0;
  };

  class FileModuleSourceProvider final : public ModuleSourceProvider {
  public:
    explicit FileModuleSourceProvider(std::filesystem::path stdlibRoot);

    std::optional<std::string> loadModuleSource(const std::string& modulePath) override;

  private:
    std::filesystem::path stdlibRoot_;
  };

  struct SymbolOrigins {
    std::unordered_map<std::string, std::string> functions;
    std::unordered_map<std::string, std::string> structs;
  };

  struct ResolvedProgram {
    ast::Module module;
    std::vector<std::string> ownedSources;
    SymbolOrigins symbolOrigins;
  };

  ResolvedProgram resolveImports(ast::Module rootModule, const CompileOptions& options,
                                 ModuleSourceProvider& provider);

  std::string formatModulePath(const std::vector<std::string>& path);

} // namespace noria
