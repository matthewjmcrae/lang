#include "noria/ModuleResolver.hpp"

#include "noria/AstClone.hpp"
#include "noria/CompilerCache.hpp"
#include "noria/Diagnostic.hpp"
#include "noria/Lexer.hpp"
#include "noria/Parser.hpp"
#include "noria/SemanticTables.hpp"
#include "noria/Types.hpp"

#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace noria {

  namespace {

    std::optional<ImplementationTag> defaultImplementationFor(
        const std::string& structName, std::size_t typeArgumentCount,
        const SymbolOrigins& symbolOrigins) {
      const auto origin = symbolOrigins.structs.find(structName);
      if (origin == symbolOrigins.structs.end()) {
        return std::nullopt;
      }

      if (origin->second == "std::sequence" &&
          (structName == "Sequence" || structName == "sequence") &&
          typeArgumentCount == 1) {
        return ImplementationTag::Arr;
      }
      if (origin->second == "std::set" && (structName == "Set" || structName == "set") &&
          typeArgumentCount == 1) {
        return ImplementationTag::Hashmap;
      }
      if (origin->second == "std::dictionary" &&
          (structName == "Dictionary" || structName == "dictionary") &&
          typeArgumentCount == 2) {
        return ImplementationTag::Hashmap;
      }
      return std::nullopt;
    }

    void normalizeType(Type& type, const SymbolOrigins& symbolOrigins) {
      if (type.kind == TypeKind::Array) {
        if (type.element) {
          normalizeType(*type.element, symbolOrigins);
        }
        return;
      }
      if (type.kind != TypeKind::Struct) {
        return;
      }

      for (Type& typeArgument : type.typeArgs) {
        normalizeType(typeArgument, symbolOrigins);
      }
      if (const auto defaultTag =
              defaultImplementationFor(type.structName, type.typeArgs.size(), symbolOrigins)) {
        type.typeArgs.push_back(Type::implementationTag(*defaultTag));
      }
    }

    void normalizeExpression(ast::Expression& expression, const SymbolOrigins& symbolOrigins);

    void normalizeStatement(ast::Statement& statement, const SymbolOrigins& symbolOrigins);

    void normalizeExpressionList(
        std::vector<std::unique_ptr<ast::Expression>>& expressions,
        const SymbolOrigins& symbolOrigins) {
      for (auto& expression : expressions) {
        normalizeExpression(*expression, symbolOrigins);
      }
    }

    void normalizeStatementList(std::vector<std::unique_ptr<ast::Statement>>& statements,
                                const SymbolOrigins& symbolOrigins) {
      for (auto& statement : statements) {
        normalizeStatement(*statement, symbolOrigins);
      }
    }

    void normalizeExpression(ast::Expression& expression, const SymbolOrigins& symbolOrigins) {
      if (auto* cast = dynamic_cast<ast::CastExpression*>(&expression)) {
        normalizeType(cast->targetType, symbolOrigins);
        normalizeExpression(*cast->expression, symbolOrigins);
      } else if (auto* unary = dynamic_cast<ast::UnaryExpression*>(&expression)) {
        normalizeExpression(*unary->operand, symbolOrigins);
      } else if (auto* binary = dynamic_cast<ast::BinaryExpression*>(&expression)) {
        normalizeExpression(*binary->left, symbolOrigins);
        normalizeExpression(*binary->right, symbolOrigins);
      } else if (auto* call = dynamic_cast<ast::CallExpression*>(&expression)) {
        normalizeExpressionList(call->arguments, symbolOrigins);
      } else if (auto* array = dynamic_cast<ast::ArrayLiteral*>(&expression)) {
        normalizeExpressionList(array->elements, symbolOrigins);
      } else if (auto* index = dynamic_cast<ast::IndexExpression*>(&expression)) {
        normalizeExpression(*index->base, symbolOrigins);
        normalizeExpression(*index->index, symbolOrigins);
      } else if (auto* literal = dynamic_cast<ast::StructLiteral*>(&expression)) {
        for (Type& typeArgument : literal->typeArgs) {
          normalizeType(typeArgument, symbolOrigins);
        }
        if (const auto defaultTag = defaultImplementationFor(
                literal->structName, literal->typeArgs.size(), symbolOrigins)) {
          literal->typeArgs.push_back(Type::implementationTag(*defaultTag));
        }
        for (auto& field : literal->fields) {
          normalizeExpression(*field.value, symbolOrigins);
        }
      } else if (auto* fieldAccess = dynamic_cast<ast::FieldAccessExpression*>(&expression)) {
        normalizeExpression(*fieldAccess->base, symbolOrigins);
      }
    }

    void normalizeStatement(ast::Statement& statement, const SymbolOrigins& symbolOrigins) {
      if (auto* returnStatement = dynamic_cast<ast::ReturnStatement*>(&statement)) {
        if (returnStatement->expression) {
          normalizeExpression(*returnStatement->expression, symbolOrigins);
        }
      } else if (auto* let = dynamic_cast<ast::LetStatement*>(&statement)) {
        if (let->declaredType) {
          normalizeType(*let->declaredType, symbolOrigins);
        }
        if (let->initializer) {
          normalizeExpression(*let->initializer, symbolOrigins);
        }
      } else if (auto* conditional = dynamic_cast<ast::IfStatement*>(&statement)) {
        normalizeExpression(*conditional->condition, symbolOrigins);
        normalizeStatementList(conditional->thenBranch, symbolOrigins);
        normalizeStatementList(conditional->elseBranch, symbolOrigins);
      } else if (auto* loop = dynamic_cast<ast::WhileStatement*>(&statement)) {
        normalizeExpression(*loop->condition, symbolOrigins);
        normalizeStatementList(loop->body, symbolOrigins);
      } else if (auto* assignment = dynamic_cast<ast::AssignmentStatement*>(&statement)) {
        normalizeExpression(*assignment->lhs, symbolOrigins);
        normalizeExpression(*assignment->rhs, symbolOrigins);
      } else if (auto* expressionStatement = dynamic_cast<ast::ExpressionStatement*>(&statement)) {
        normalizeExpression(*expressionStatement->expression, symbolOrigins);
      }
    }

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

    std::optional<const ast::Function*> findFunction(const ast::Module& module,
                                                     const std::string& name) {
      for (const auto& function : module.functions) {
        if (function.name == name) {
          return &function;
        }
      }
      return std::nullopt;
    }

    std::optional<const ast::StructDecl*> findStruct(const ast::Module& module,
                                                     const std::string& name) {
      for (const auto& structDecl : module.structs) {
        if (structDecl.name == name) {
          return &structDecl;
        }
      }
      return std::nullopt;
    }

    std::vector<ast::Function> cloneFunctionFamily(const ast::Module& module,
                                                   const std::string& name) {
      std::vector<ast::Function> family;
      for (const auto& function : module.functions) {
        if (function.name == name) {
          family.push_back(ast::cloneFunction(function));
        }
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

    std::unordered_set<std::string> collectStructExportNames(const ast::Module& module,
                                                             const std::string& modulePath) {
      std::unordered_set<std::string> structNames;
      for (const auto& structDecl : module.structs) {
        if (!structNames.insert(structDecl.name).second) {
          throwResolverError(structDecl.location, modulePath,
                             "duplicate struct '" + structDecl.name + "'");
        }
      }
      return structNames;
    }

    void validateConcreteFunctionExport(
        const ast::Function& function, std::unordered_set<std::string>& exportedFunctionNames,
        const std::unordered_map<std::string, std::vector<const ast::Function*>>& genericFamilies,
        const std::string& modulePath) {
      if (exportedFunctionNames.contains(function.name) ||
          genericFamilies.contains(function.name)) {
        throwResolverError(function.location, modulePath,
                           "duplicate function '" + function.name + "'");
      }
      exportedFunctionNames.insert(function.name);
    }

    void validateGenericFamilyMember(const ast::Function& function,
                                     const std::vector<const ast::Function*>& family,
                                     const std::string& modulePath) {
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
    }

    void validateGenericFunctionExport(
        const ast::Function& function, std::unordered_set<std::string>& exportedFunctionNames,
        std::unordered_map<std::string, std::vector<const ast::Function*>>& genericFamilies,
        const std::string& modulePath) {
      if (exportedFunctionNames.contains(function.name)) {
        throwResolverError(function.location, modulePath,
                           "duplicate function '" + function.name + "'");
      }

      std::vector<const ast::Function*>& family = genericFamilies[function.name];
      validateGenericFamilyMember(function, family, modulePath);
      family.push_back(&function);
    }

    void validateFunctionExports(const ast::Module& module,
                                 const std::unordered_set<std::string>& structNames,
                                 const std::string& modulePath) {
      std::unordered_map<std::string, std::vector<const ast::Function*>> genericFamilies;
      std::unordered_set<std::string> exportedFunctionNames;

      for (const auto& function : module.functions) {
        if (structNames.contains(function.name)) {
          throwResolverError(function.location, modulePath,
                             "export '" + function.name + "' is both a function and a struct");
        }

        if (function.typeParams.empty()) {
          validateConcreteFunctionExport(function, exportedFunctionNames, genericFamilies,
                                         modulePath);
          continue;
        }

        validateGenericFunctionExport(function, exportedFunctionNames, genericFamilies, modulePath);
      }
    }

    void validateModuleExports(const ast::Module& module, const std::string& modulePath) {
      const std::unordered_set<std::string> structNames =
          collectStructExportNames(module, modulePath);
      validateFunctionExports(module, structNames, modulePath);

      validateImportLists(module.imports, modulePath);
    }

    class Resolver {
    public:
      Resolver(ModuleSourceProvider& provider, std::vector<std::string>& ownedSources,
               CompilerCache* compilerCache, std::string stdlibRootKey)
          : provider_(provider), ownedSources_(ownedSources), compilerCache_(compilerCache),
            stdlibRootKey_(std::move(stdlibRootKey)) {}

      const ast::Module& loadModule(const std::vector<std::string>& path, SourceLocation location) {
        const std::string modulePath = formatModulePath(path);

        if (const ast::Module* cached = cachedModule(modulePath)) {
          return *cached;
        }

        rejectImportCycle(modulePath, location);
        validateLoadableModulePath(path, modulePath, location);

        if (compilerCache_ != nullptr && isStdlibModulePath(modulePath)) {
          const std::string cacheKey = parsedStdlibModuleCacheKey(stdlibRootKey_, modulePath);
          if (std::optional<ast::Module> cached =
                  compilerCache_->cloneParsedStdlibModule(cacheKey)) {
            ParsedModule parsed;
            parsed.module = std::move(*cached);
            auto [iterator, inserted] = cache_.emplace(modulePath, std::move(parsed));
            (void)inserted;
            return iterator->second.module;
          }
        }

        const ParsedSourceModule parsedSource = parseModuleSource(modulePath, location);
        ast::Module module = ast::cloneModule(parsedSource.module);

        beginVisit(modulePath);

        for (const auto& importDecl : module.imports) {
          loadModule(importDecl.path, importDecl.location);
        }

        endVisit(modulePath);

        ParsedModule parsed;
        parsed.module = std::move(module);
        if (compilerCache_ != nullptr && isStdlibModulePath(modulePath)) {
          const std::string cacheKey = parsedStdlibModuleCacheKey(stdlibRootKey_, modulePath);
          compilerCache_->storeParsedStdlibModule(cacheKey, parsed.module);
        }
        auto [iterator, inserted] = cache_.emplace(modulePath, std::move(parsed));
        (void)inserted;
        return iterator->second.module;
      }

    private:
      struct ParsedModule {
        ast::Module module;
      };

      struct ParsedSourceModule {
        ast::Module module;
        std::size_t sourceBytes = 0;
      };

      const ast::Module* cachedModule(const std::string& modulePath) {
        const auto cached = cache_.find(modulePath);
        if (cached == cache_.end()) {
          return nullptr;
        }
        return &cached->second.module;
      }

      void rejectImportCycle(const std::string& modulePath, SourceLocation location) const {
        if (!visiting_.contains(modulePath)) {
          return;
        }

        // The stack is kept in import order so diagnostics can show the full cycle path.
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

      void validateLoadableModulePath(const std::vector<std::string>& path,
                                      const std::string& modulePath,
                                      SourceLocation location) const {
        if (path[0] != "std") {
          throwResolverError(location, modulePath,
                             "unsupported module root '" + path[0] +
                                 "'; only 'std' imports are supported");
        }

        if (path.size() < 2) {
          throwResolverError(location, modulePath, "unsupported module path '" + modulePath + "'");
        }
      }

      ParsedSourceModule parseModuleSource(const std::string& modulePath, SourceLocation location) {
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
        return ParsedSourceModule{std::move(module), source->size()};
      }

      void beginVisit(const std::string& modulePath) {
        visiting_.insert(modulePath);
        visitingStack_.push_back(modulePath);
      }

      void endVisit(const std::string& modulePath) {
        visitingStack_.pop_back();
        visiting_.erase(modulePath);
      }

      ModuleSourceProvider& provider_;
      std::vector<std::string>& ownedSources_;
      CompilerCache* compilerCache_ = nullptr;
      std::string stdlibRootKey_;
      std::unordered_map<std::string, ParsedModule> cache_;
      std::unordered_set<std::string> visiting_;
      std::vector<std::string> visitingStack_;
    };

    void mergeImportedName(ast::Module& merged, const ast::Module& sourceModule,
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

        for (ast::Function& function : cloneFunctionFamily(sourceModule, importedName.name)) {
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

        merged.structs.push_back(
            ast::cloneStructDecl(**findStruct(sourceModule, importedName.name)));
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

    void mergeHiddenContainerOperation(ast::Module& merged, const ast::Module& sourceModule,
                                       const std::string& modulePath, StandardContainer container,
                                       ContainerOperation operation, SymbolOrigins& symbolOrigins) {
      const std::string sourceName(containerOperationSourceName(container, operation));
      const std::string hiddenName(containerOperationHiddenName(container, operation));
      if (sourceName.empty() || hiddenName.empty() || symbolOrigins.functions.contains(hiddenName)) {
        return;
      }

      for (ast::Function& function : cloneFunctionFamily(sourceModule, sourceName)) {
        function.name = hiddenName;
        merged.functions.push_back(std::move(function));
      }
      symbolOrigins.functions.emplace(hiddenName, modulePath);
      symbolOrigins.hiddenFunctions.insert(hiddenName);
    }

    void mergeHiddenContainerOperations(ast::Module& merged, const ast::Module& sourceModule,
                                        const std::string& modulePath,
                                        const ast::ImportedName& importedName,
                                        SymbolOrigins& symbolOrigins) {
      const StandardContainerInfo* info = standardContainerInfo(modulePath, importedName.name);
      if (info == nullptr || !findStruct(sourceModule, importedName.name)) {
        return;
      }

      switch (info->kind) {
      case StandardContainer::Sequence:
        mergeHiddenContainerOperation(merged, sourceModule, modulePath, info->kind,
                                      ContainerOperation::New, symbolOrigins);
        mergeHiddenContainerOperation(merged, sourceModule, modulePath, info->kind,
                                      ContainerOperation::Get, symbolOrigins);
        mergeHiddenContainerOperation(merged, sourceModule, modulePath, info->kind,
                                      ContainerOperation::Set, symbolOrigins);
        mergeHiddenContainerOperation(merged, sourceModule, modulePath, info->kind,
                                      ContainerOperation::Drop, symbolOrigins);
        mergeHiddenContainerOperation(merged, sourceModule, modulePath, info->kind,
                                      ContainerOperation::Clone, symbolOrigins);
        return;
      case StandardContainer::Dictionary:
        mergeHiddenContainerOperation(merged, sourceModule, modulePath, info->kind,
                                      ContainerOperation::New, symbolOrigins);
        mergeHiddenContainerOperation(merged, sourceModule, modulePath, info->kind,
                                      ContainerOperation::Get, symbolOrigins);
        mergeHiddenContainerOperation(merged, sourceModule, modulePath, info->kind,
                                      ContainerOperation::Contains, symbolOrigins);
        mergeHiddenContainerOperation(merged, sourceModule, modulePath, info->kind,
                                      ContainerOperation::Insert, symbolOrigins);
        mergeHiddenContainerOperation(merged, sourceModule, modulePath, info->kind,
                                      ContainerOperation::Drop, symbolOrigins);
        mergeHiddenContainerOperation(merged, sourceModule, modulePath, info->kind,
                                      ContainerOperation::Clone, symbolOrigins);
        return;
      case StandardContainer::Set:
        mergeHiddenContainerOperation(merged, sourceModule, modulePath, info->kind,
                                      ContainerOperation::New, symbolOrigins);
        mergeHiddenContainerOperation(merged, sourceModule, modulePath, info->kind,
                                      ContainerOperation::Contains, symbolOrigins);
        mergeHiddenContainerOperation(merged, sourceModule, modulePath, info->kind,
                                      ContainerOperation::Drop, symbolOrigins);
        mergeHiddenContainerOperation(merged, sourceModule, modulePath, info->kind,
                                      ContainerOperation::Clone, symbolOrigins);
        return;
      }
    }

    void mergeStdlibDependencies(ast::Module& merged, const ast::Module& stdlibModule,
                                 Resolver& resolver, SymbolOrigins& symbolOrigins,
                                 std::unordered_set<std::string>& mergedStdlibModules,
                                 const std::string& stdlibModulePath) {
      if (!mergedStdlibModules.insert(stdlibModulePath).second) {
        return;
      }

      for (const auto& importDecl : stdlibModule.imports) {
        rejectInternalImport(importDecl.path, importDecl.location, stdlibModulePath);

        const ast::Module& dependency = resolver.loadModule(importDecl.path, importDecl.location);
        const std::string dependencyPath = formatModulePath(importDecl.path);

        for (const auto& importedName : importDecl.names) {
          mergeImportedName(merged, dependency, dependencyPath, importedName, symbolOrigins);
        }

        if (isStdlibModulePath(dependencyPath)) {
          mergeStdlibDependencies(merged, dependency, resolver, symbolOrigins, mergedStdlibModules,
                                  dependencyPath);
        }
      }
    }

    void mergeImportsFromModule(ast::Module& merged, const ast::Module& importSource,
                                const std::vector<ast::ImportDecl>& imports, Resolver& resolver,
                                SymbolOrigins& symbolOrigins,
                                const std::string& importingModulePath,
                                std::unordered_set<std::string>& mergedStdlibModules) {
      (void)importSource;
      for (const auto& importDecl : imports) {
        rejectInternalImport(importDecl.path, importDecl.location, importingModulePath);

        const ast::Module& dependency = resolver.loadModule(importDecl.path, importDecl.location);
        const std::string modulePath = formatModulePath(importDecl.path);

        for (const auto& importedName : importDecl.names) {
          mergeImportedName(merged, dependency, modulePath, importedName, symbolOrigins);
          mergeHiddenContainerOperations(merged, dependency, modulePath, importedName,
                                         symbolOrigins);
        }

        if (isStdlibModulePath(modulePath)) {
          mergeStdlibDependencies(merged, dependency, resolver, symbolOrigins, mergedStdlibModules,
                                  modulePath);
        }
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
                                 ModuleSourceProvider& provider, CompilerCache* compilerCache) {
    ResolvedProgram resolved;
    resolved.module.imports = std::move(rootModule.imports);
    resolved.module.structs = std::move(rootModule.structs);
    resolved.module.functions = std::move(rootModule.functions);

    if (resolved.module.imports.empty()) {
      return resolved;
    }

    validateImportLists(resolved.module.imports, "");

    std::string stdlibRootKey;
    if (compilerCache != nullptr && options.stdlibRoot.has_value()) {
      stdlibRootKey = stdlibRootCacheKey(*options.stdlibRoot);
    }

    Resolver resolver(provider, resolved.ownedSources, compilerCache, std::move(stdlibRootKey));
    SymbolOrigins symbolOrigins;
    std::unordered_set<std::string> mergedStdlibModules;
    for (const auto& function : resolved.module.functions) {
      symbolOrigins.functions.emplace(function.name, "");
    }
    for (const auto& structDecl : resolved.module.structs) {
      symbolOrigins.structs.emplace(structDecl.name, "");
    }
    mergeImportsFromModule(resolved.module, resolved.module, resolved.module.imports, resolver,
                           symbolOrigins, "", mergedStdlibModules);
    resolved.symbolOrigins = std::move(symbolOrigins);
    return resolved;
  }

  void applyDefaultAdtImplementations(ast::Module& module,
                                      const SymbolOrigins& symbolOrigins) {
    for (auto& structDecl : module.structs) {
      for (auto& field : structDecl.fields) {
        normalizeType(field.type, symbolOrigins);
      }
    }

    for (auto& function : module.functions) {
      if (function.returnType) {
        normalizeType(*function.returnType, symbolOrigins);
      }
      for (auto& parameter : function.parameters) {
        normalizeType(parameter.type, symbolOrigins);
      }
      for (auto& statement : function.body) {
        normalizeStatement(*statement, symbolOrigins);
      }
    }
  }

} // namespace noria
