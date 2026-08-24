#include "noria/ModuleResolver.hpp"

#include "noria/Diagnostic.hpp"
#include "noria/Lexer.hpp"
#include "noria/Parser.hpp"
#include "noria/Types.hpp"

#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace noria {

  namespace {

    [[noreturn]] void throwResolverError(SourceLocation location, const std::string& modulePath,
                                         std::string_view message) {
      SourceLocation diagnosticLocation = location;
      if (!modulePath.empty()) {
        diagnosticLocation.file = modulePath;
      }
      throw CompileError(formatDiagnostic(diagnosticLocation, DiagnosticStage::Import, message));
    }

    std::optional<std::string> moduleFileName(const std::vector<std::string>& path) {
      if (path.empty() || path[0] != "std" || path.size() < 2) {
        return std::nullopt;
      }

      std::ostringstream fileName;
      for (std::size_t index = 1; index < path.size(); ++index) {
        if (index != 1) {
          fileName << '/';
        }
        fileName << path[index];
      }
      fileName << ".noria";
      return fileName.str();
    }

    bool isStdlibModulePath(const std::string& modulePath) {
      return modulePath.rfind("std::", 0) == 0;
    }

    void rejectInternalImport(const std::vector<std::string>& importPath, SourceLocation location,
                              const std::string& importingModulePath) {
      if (importPath.size() < 2 || importPath[1] != "internal") {
        return;
      }

      if (isStdlibModulePath(importingModulePath)) {
        return;
      }

      const std::string modulePath = formatModulePath(importPath);
      throwResolverError(location, modulePath,
                         "module '" + modulePath + "' is internal and cannot be imported");
    }

    std::optional<ast::Function*> findFunction(ast::Module& module, const std::string& name) {
      for (auto& function : module.functions) {
        if (function.name == name) {
          return &function;
        }
      }
      return std::nullopt;
    }

    std::optional<ast::StructDecl*> findStruct(ast::Module& module, const std::string& name) {
      for (auto& structDecl : module.structs) {
        if (structDecl.name == name) {
          return &structDecl;
        }
      }
      return std::nullopt;
    }

    std::vector<ast::Function> takeFunctionFamily(ast::Module& module, const std::string& name) {
      std::vector<ast::Function> family;
      for (auto iterator = module.functions.begin(); iterator != module.functions.end();) {
        if (iterator->name == name) {
          family.push_back(std::move(*iterator));
          iterator = module.functions.erase(iterator);
          continue;
        }
        ++iterator;
      }

      if (family.empty()) {
        throw CompileError("internal error: missing function '" + name + "'");
      }

      return family;
    }

    void validateImportLists(const std::vector<ast::ImportDecl>& imports,
                             const std::string& modulePath) {
      for (const auto& importDecl : imports) {
        std::unordered_set<std::string> seenNames;
        for (const auto& importedName : importDecl.names) {
          if (!seenNames.insert(importedName.name).second) {
            throwResolverError(importedName.location, modulePath,
                               "duplicate import '" + importedName.name + "'");
          }
        }
      }
    }

    void validateModuleExports(const ast::Module& module, const std::string& modulePath) {
      std::unordered_set<std::string> structNames;
      for (const auto& structDecl : module.structs) {
        if (!structNames.insert(structDecl.name).second) {
          throwResolverError(structDecl.location, modulePath,
                             "duplicate struct '" + structDecl.name + "'");
        }
      }

      std::unordered_map<std::string, std::vector<const ast::Function*>> genericFamilies;
      std::unordered_set<std::string> exportedFunctionNames;

      for (const auto& function : module.functions) {
        if (structNames.contains(function.name)) {
          throwResolverError(function.location, modulePath,
                             "export '" + function.name + "' is both a function and a struct");
        }

        if (function.typeParams.empty()) {
          if (exportedFunctionNames.contains(function.name) ||
              genericFamilies.contains(function.name)) {
            throwResolverError(function.location, modulePath,
                               "duplicate function '" + function.name + "'");
          }
          exportedFunctionNames.insert(function.name);
          continue;
        }

        if (exportedFunctionNames.contains(function.name)) {
          throwResolverError(function.location, modulePath,
                             "duplicate function '" + function.name + "'");
        }

        std::vector<const ast::Function*>& family = genericFamilies[function.name];
        for (const ast::Function* candidate : family) {
          if (function.implTag && candidate->implTag && *function.implTag == *candidate->implTag) {
            throwResolverError(function.location, modulePath,
                               "duplicate implementation '" +
                                   std::string(implementationTagName(*function.implTag)) +
                                   "' for function '" + function.name + "'");
          }
          if (static_cast<bool>(function.implTag) != static_cast<bool>(candidate->implTag)) {
            throwResolverError(function.location, modulePath,
                               "function '" + function.name +
                                   "' mixes tagged and untagged implementations");
          }
          if (!function.implTag && !candidate->implTag) {
            throwResolverError(function.location, modulePath,
                               "duplicate function '" + function.name + "'");
          }
        }
        family.push_back(&function);
      }

      validateImportLists(module.imports, modulePath);
    }

    ast::StructDecl takeStruct(ast::Module& module, const std::string& name) {
      for (auto iterator = module.structs.begin(); iterator != module.structs.end(); ++iterator) {
        if (iterator->name == name) {
          ast::StructDecl moved = std::move(*iterator);
          module.structs.erase(iterator);
          return moved;
        }
      }
      throw CompileError("internal error: missing struct '" + name + "'");
    }

    class Resolver {
    public:
      Resolver(ModuleSourceProvider& provider, std::vector<std::string>& ownedSources)
          : provider_(provider), ownedSources_(ownedSources) {}

      ast::Module& loadModule(const std::vector<std::string>& path, SourceLocation location) {
        const std::string modulePath = formatModulePath(path);

        if (const auto cached = cache_.find(modulePath); cached != cache_.end()) {
          return cached->second.module;
        }

        if (visiting_.contains(modulePath)) {
          std::ostringstream cyclePath;
          bool first = true;
          for (const auto& segment : visitingStack_) {
            if (!first) {
              cyclePath << " -> ";
            }
            first = false;
            cyclePath << segment;
          }
          cyclePath << " -> " << modulePath;
          throwResolverError(location, modulePath, "import cycle detected: " + cyclePath.str());
        }

        if (path[0] != "std") {
          throwResolverError(location, modulePath,
                             "unsupported module root '" + path[0] +
                                 "'; only 'std' imports are supported");
        }

        if (path.size() < 2) {
          throwResolverError(location, modulePath, "unsupported module path '" + modulePath + "'");
        }

        const std::optional<std::string> source = provider_.loadModuleSource(modulePath);
        if (!source.has_value()) {
          throwResolverError(location, modulePath, "unknown module '" + modulePath + "'");
        }

        ownedSources_.push_back(*source);
        const std::string_view sourceView = ownedSources_.back();

        Lexer lexer(sourceView, modulePath);
        const std::vector<Token> tokens = lexer.lex();
        Parser parser(tokens);
        ast::Module module = parser.parseModule();
        validateModuleExports(module, modulePath);

        visiting_.insert(modulePath);
        visitingStack_.push_back(modulePath);

        for (const auto& importDecl : module.imports) {
          loadModule(importDecl.path, importDecl.location);
        }

        visitingStack_.pop_back();
        visiting_.erase(modulePath);

        ParsedModule parsed;
        parsed.module = std::move(module);
        parsed.source = ownedSources_.back();
        auto [iterator, inserted] = cache_.emplace(modulePath, std::move(parsed));
        (void)inserted;
        return iterator->second.module;
      }

    private:
      struct ParsedModule {
        std::string source;
        ast::Module module;
      };

      ModuleSourceProvider& provider_;
      std::vector<std::string>& ownedSources_;
      std::unordered_map<std::string, ParsedModule> cache_;
      std::unordered_set<std::string> visiting_;
      std::vector<std::string> visitingStack_;
    };

    void mergeImportedName(ast::Module& merged, ast::Module& sourceModule,
                           const std::string& modulePath, const ast::ImportedName& importedName,
                           SymbolOrigins& symbolOrigins) {
      if (findFunction(sourceModule, importedName.name)) {
        if (const auto existingOrigin = symbolOrigins.functions.find(importedName.name);
            existingOrigin != symbolOrigins.functions.end()) {
          if (existingOrigin->second == modulePath) {
            return;
          }
          throwResolverError(importedName.location, modulePath,
                             "duplicate symbol '" + importedName.name + "'");
        }

        for (ast::Function& function : takeFunctionFamily(sourceModule, importedName.name)) {
          merged.functions.push_back(std::move(function));
        }
        symbolOrigins.functions.emplace(importedName.name, modulePath);
        return;
      }

      if (findStruct(sourceModule, importedName.name)) {
        if (const auto existingOrigin = symbolOrigins.structs.find(importedName.name);
            existingOrigin != symbolOrigins.structs.end()) {
          if (existingOrigin->second == modulePath) {
            return;
          }
          throwResolverError(importedName.location, modulePath,
                             "duplicate symbol '" + importedName.name + "'");
        }

        merged.structs.push_back(takeStruct(sourceModule, importedName.name));
        symbolOrigins.structs.emplace(importedName.name, modulePath);
        return;
      }

      if (const auto existingFunctionOrigin = symbolOrigins.functions.find(importedName.name);
          existingFunctionOrigin != symbolOrigins.functions.end() &&
          existingFunctionOrigin->second == modulePath) {
        return;
      }

      if (const auto existingStructOrigin = symbolOrigins.structs.find(importedName.name);
          existingStructOrigin != symbolOrigins.structs.end() &&
          existingStructOrigin->second == modulePath) {
        return;
      }

      throwResolverError(importedName.location, modulePath,
                         "module '" + modulePath + "' does not export '" + importedName.name + "'");
    }

    void mergeImportsFromModule(ast::Module& merged, const ast::Module& importSource,
                                const std::vector<ast::ImportDecl>& imports, Resolver& resolver,
                                SymbolOrigins& symbolOrigins,
                                const std::string& importingModulePath) {
      (void)importSource;
      for (const auto& importDecl : imports) {
        rejectInternalImport(importDecl.path, importDecl.location, importingModulePath);

        ast::Module& dependency = resolver.loadModule(importDecl.path, importDecl.location);

        for (const auto& importedName : importDecl.names) {
          const std::string modulePath = formatModulePath(importDecl.path);
          mergeImportedName(merged, dependency, modulePath, importedName, symbolOrigins);
        }

        mergeImportsFromModule(merged, dependency, dependency.imports, resolver, symbolOrigins,
                               formatModulePath(importDecl.path));
      }
    }

  } // namespace

  std::string formatModulePath(const std::vector<std::string>& path) {
    std::ostringstream formatted;
    for (std::size_t index{}; index < path.size(); ++index) {
      if (index != 0) {
        formatted << "::";
      }
      formatted << path[index];
    }
    return formatted.str();
  }

  FileModuleSourceProvider::FileModuleSourceProvider(std::filesystem::path stdlibRoot)
      : stdlibRoot_(std::move(stdlibRoot)) {}

  std::optional<std::string>
  FileModuleSourceProvider::loadModuleSource(const std::string& modulePath) {
    std::vector<std::string> pathSegments;
    std::size_t start = 0;
    while (start < modulePath.size()) {
      const std::size_t separator = modulePath.find("::", start);
      if (separator == std::string::npos) {
        pathSegments.push_back(modulePath.substr(start));
        break;
      }
      pathSegments.push_back(modulePath.substr(start, separator - start));
      start = separator + 2;
    }

    const std::optional<std::string> fileName = moduleFileName(pathSegments);
    if (!fileName.has_value()) {
      return std::nullopt;
    }

    const std::filesystem::path filePath = stdlibRoot_ / *fileName;
    std::ifstream file(filePath);
    if (!file) {
      return std::nullopt;
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
  }

  ResolvedProgram resolveImports(ast::Module rootModule, const CompileOptions& options,
                                 ModuleSourceProvider& provider) {
    ResolvedProgram resolved;
    resolved.module.imports = std::move(rootModule.imports);
    resolved.module.structs = std::move(rootModule.structs);
    resolved.module.functions = std::move(rootModule.functions);

    if (resolved.module.imports.empty()) {
      return resolved;
    }

    validateImportLists(resolved.module.imports, "");

    Resolver resolver(provider, resolved.ownedSources);
    SymbolOrigins symbolOrigins;
    for (const auto& function : resolved.module.functions) {
      symbolOrigins.functions.emplace(function.name, "");
    }
    for (const auto& structDecl : resolved.module.structs) {
      symbolOrigins.structs.emplace(structDecl.name, "");
    }
    mergeImportsFromModule(resolved.module, resolved.module, resolved.module.imports, resolver,
                           symbolOrigins, "");
    resolved.symbolOrigins = std::move(symbolOrigins);
    return resolved;
  }

} // namespace noria
