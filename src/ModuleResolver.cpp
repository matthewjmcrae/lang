#include "noria/ModuleResolver.hpp"

#include "noria/Diagnostic.hpp"
#include "noria/Lexer.hpp"
#include "noria/Parser.hpp"

#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace noria {

  namespace {

    [[noreturn]] void throwResolverError(SourceLocation location, const std::string& modulePath,
                                         std::string_view message) {
      std::ostringstream formatted;
      formatted << modulePath << ": " << location.line << ":" << location.column << ": " << message;
      throw CompileError(formatted.str());
    }

    std::optional<std::string> moduleFileName(const std::vector<std::string>& path) {
      if (path.size() != 2 || path[0] != "std") {
        return std::nullopt;
      }
      return path[1] + ".noria";
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

    ast::Function takeFunction(ast::Module& module, const std::string& name) {
      for (auto iterator = module.functions.begin(); iterator != module.functions.end();
           ++iterator) {
        if (iterator->name == name) {
          ast::Function moved = std::move(*iterator);
          module.functions.erase(iterator);
          return moved;
        }
      }
      throw CompileError("internal error: missing function '" + name + "'");
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

        if (path.size() != 2) {
          throwResolverError(location, modulePath, "unsupported module path '" + modulePath + "'");
        }

        const std::optional<std::string> source = provider_.loadModuleSource(modulePath);
        if (!source.has_value()) {
          throwResolverError(location, modulePath, "unknown module '" + modulePath + "'");
        }

        ownedSources_.push_back(*source);
        const std::string_view sourceView = ownedSources_.back();

        Lexer lexer(sourceView);
        const std::vector<Token> tokens = lexer.lex();
        Parser parser(tokens);
        ast::Module module = parser.parseModule();

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
                           std::unordered_map<std::string, std::string>& symbolOrigins) {
      if (const auto existingOrigin = symbolOrigins.find(importedName.name);
          existingOrigin != symbolOrigins.end()) {
        if (existingOrigin->second == modulePath) {
          return;
        }
        throwResolverError(importedName.location, modulePath,
                           "duplicate symbol '" + importedName.name + "'");
      }

      if (findFunction(sourceModule, importedName.name)) {
        merged.functions.push_back(takeFunction(sourceModule, importedName.name));
        symbolOrigins.emplace(importedName.name, modulePath);
        return;
      }

      if (findStruct(sourceModule, importedName.name)) {
        merged.structs.push_back(takeStruct(sourceModule, importedName.name));
        symbolOrigins.emplace(importedName.name, modulePath);
        return;
      }

      throwResolverError(importedName.location, modulePath,
                         "module '" + modulePath + "' does not export '" + importedName.name + "'");
    }

    void mergeImportsFromModule(ast::Module& merged, const ast::Module& importSource,
                                const std::vector<ast::ImportDecl>& imports, Resolver& resolver,
                                std::unordered_map<std::string, std::string>& symbolOrigins) {
      (void)importSource;
      for (const auto& importDecl : imports) {
        ast::Module& dependency = resolver.loadModule(importDecl.path, importDecl.location);

        for (const auto& importedName : importDecl.names) {
          const std::string modulePath = formatModulePath(importDecl.path);
          mergeImportedName(merged, dependency, modulePath, importedName, symbolOrigins);
        }

        mergeImportsFromModule(merged, dependency, dependency.imports, resolver, symbolOrigins);
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
    (void)options;

    ResolvedProgram resolved;
    resolved.module.imports = std::move(rootModule.imports);
    resolved.module.structs = std::move(rootModule.structs);
    resolved.module.functions = std::move(rootModule.functions);

    if (resolved.module.imports.empty()) {
      return resolved;
    }

    Resolver resolver(provider, resolved.ownedSources);
    std::unordered_map<std::string, std::string> symbolOrigins;
    mergeImportsFromModule(resolved.module, resolved.module, resolved.module.imports, resolver,
                           symbolOrigins);
    return resolved;
  }

} // namespace noria
