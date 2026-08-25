#include "noria/CompilerCache.hpp"

#include "noria/AstClone.hpp"
#include "noria/AstVisitor.hpp"

#include <algorithm>
#include <sstream>
#include <system_error>
#include <utility>

namespace noria {

  namespace {

    std::size_t stringWeight(const std::string& value) {
      return sizeof(std::string) + value.size();
    }

    std::size_t typeWeight(const Type& type) {
      if (type.kind == TypeKind::Array) {
        return 1 + (type.element ? typeWeight(*type.element) : 0);
      }

      if (type.kind == TypeKind::Struct) {
        std::size_t weight = stringWeight(type.structName);
        for (const Type& typeArg : type.typeArgs) {
          weight += typeWeight(typeArg);
        }
        return weight;
      }

      if (type.kind == TypeKind::TypeParam) {
        return stringWeight(type.typeParamName);
      }

      return 1;
    }

    std::size_t expressionWeight(const ast::Expression& expression);
    std::size_t statementWeight(const ast::Statement& statement);

    class AstWeightVisitor final : public ast::AstVisitor {
    public:
      std::size_t result() const { return result_; }

      void visit(const ast::IntegerLiteral& node) override {
        result_ = sizeof(node) + stringWeight(node.location.file);
      }

      void visit(const ast::FloatLiteral& node) override {
        result_ = sizeof(node) + stringWeight(node.location.file);
      }

      void visit(const ast::StringLiteral& node) override {
        result_ = sizeof(node) + stringWeight(node.value) + stringWeight(node.location.file);
      }

      void visit(const ast::BoolLiteral& node) override {
        result_ = sizeof(node) + stringWeight(node.location.file);
      }

      void visit(const ast::UnaryExpression& node) override {
        result_ = sizeof(node) + stringWeight(node.location.file) + expressionWeight(*node.operand);
      }

      void visit(const ast::CastExpression& node) override {
        result_ = sizeof(node) + stringWeight(node.location.file) + typeWeight(node.targetType) +
                  expressionWeight(*node.expression);
      }

      void visit(const ast::BinaryExpression& node) override {
        result_ = sizeof(node) + stringWeight(node.location.file) + expressionWeight(*node.left) +
                  expressionWeight(*node.right);
      }

      void visit(const ast::IdentifierExpression& node) override {
        result_ = sizeof(node) + stringWeight(node.name) + stringWeight(node.location.file);
      }

      void visit(const ast::CallExpression& node) override {
        result_ = sizeof(node) + stringWeight(node.callee) + stringWeight(node.location.file);
        for (const auto& argument : node.arguments) {
          result_ += sizeof(argument) + expressionWeight(*argument);
        }
      }

      void visit(const ast::ArrayLiteral& node) override {
        result_ = sizeof(node) + stringWeight(node.location.file);
        for (const auto& element : node.elements) {
          result_ += sizeof(element) + expressionWeight(*element);
        }
      }

      void visit(const ast::IndexExpression& node) override {
        result_ = sizeof(node) + stringWeight(node.location.file) + expressionWeight(*node.base) +
                  expressionWeight(*node.index);
      }

      void visit(const ast::StructLiteral& node) override {
        result_ = sizeof(node) + stringWeight(node.structName) + stringWeight(node.location.file);
        for (const Type& typeArg : node.typeArgs) {
          result_ += typeWeight(typeArg);
        }
        for (const auto& field : node.fields) {
          result_ += stringWeight(field.name) + stringWeight(field.location.file) +
                     expressionWeight(*field.value);
        }
      }

      void visit(const ast::FieldAccessExpression& node) override {
        result_ = sizeof(node) + stringWeight(node.fieldName) + stringWeight(node.location.file) +
                  expressionWeight(*node.base);
      }

      void visit(const ast::ReturnStatement& node) override {
        result_ = sizeof(node) + stringWeight(node.location.file);
        if (node.expression) {
          result_ += expressionWeight(*node.expression);
        }
      }

      void visit(const ast::LetStatement& node) override {
        result_ = sizeof(node) + stringWeight(node.name) + stringWeight(node.location.file);
        if (node.declaredType) {
          result_ += typeWeight(*node.declaredType);
        }
        if (node.initializer) {
          result_ += expressionWeight(*node.initializer);
        }
      }

      void visit(const ast::IfStatement& node) override {
        result_ =
            sizeof(node) + stringWeight(node.location.file) + expressionWeight(*node.condition);
        for (const auto& statement : node.thenBranch) {
          result_ += sizeof(statement) + statementWeight(*statement);
        }
        for (const auto& statement : node.elseBranch) {
          result_ += sizeof(statement) + statementWeight(*statement);
        }
      }

      void visit(const ast::WhileStatement& node) override {
        result_ =
            sizeof(node) + stringWeight(node.location.file) + expressionWeight(*node.condition);
        for (const auto& statement : node.body) {
          result_ += sizeof(statement) + statementWeight(*statement);
        }
      }

      void visit(const ast::AssignmentStatement& node) override {
        result_ = sizeof(node) + stringWeight(node.location.file) + expressionWeight(*node.lhs) +
                  expressionWeight(*node.rhs);
      }

      void visit(const ast::ExpressionStatement& node) override {
        result_ =
            sizeof(node) + stringWeight(node.location.file) + expressionWeight(*node.expression);
      }

    private:
      std::size_t result_ = 0;
    };

    std::size_t expressionWeight(const ast::Expression& expression) {
      AstWeightVisitor visitor;
      expression.accept(visitor);
      return visitor.result();
    }

    std::size_t statementWeight(const ast::Statement& statement) {
      AstWeightVisitor visitor;
      statement.accept(visitor);
      return visitor.result();
    }

    std::size_t functionWeight(const ast::Function& function) {
      std::size_t weight = sizeof(function) + stringWeight(function.name) +
                           stringWeight(function.location.file) + typeWeight(function.returnType);
      for (const auto& typeParam : function.typeParams) {
        weight += sizeof(typeParam) + stringWeight(typeParam.name) +
                  stringWeight(typeParam.location.file);
      }
      for (const auto& parameter : function.parameters) {
        weight += sizeof(parameter) + stringWeight(parameter.name) +
                  stringWeight(parameter.location.file) + typeWeight(parameter.type);
      }
      for (const auto& statement : function.body) {
        weight += sizeof(statement) + statementWeight(*statement);
      }
      return weight;
    }

    std::size_t specializationWeight(const ast::Function& function,
                                     const std::vector<Type>& typeArgs) {
      std::size_t weight = functionWeight(function);
      for (const Type& typeArg : typeArgs) {
        weight += typeWeight(typeArg);
      }
      return std::max<std::size_t>(1, weight);
    }

  } // namespace

  CompilerCache::CompilerCache()
      : parsedStdlibModules_(kMaxParsedStdlibModules),
        stdlibSpecializations_(kMaxStdlibSpecializations) {}

  std::optional<ast::Module> CompilerCache::cloneParsedStdlibModule(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    return parsedStdlibModules_.get(
        key, [](const CachedParsedModule& cached) { return ast::cloneModule(cached.module); });
  }

  void CompilerCache::storeParsedStdlibModule(const std::string& key, const ast::Module& module) {
    CachedParsedModule cached;
    cached.module = ast::cloneModule(module);

    std::lock_guard<std::mutex> lock(mutex_);
    parsedStdlibModules_.put(key, std::move(cached));
  }

  std::optional<CachedFunctionSpecialization>
  CompilerCache::cloneStdlibFunctionSpecialization(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    return stdlibSpecializations_.get(key, [](const CachedSpecialization& cached) {
      if (cached.kind != CachedSpecialization::Kind::Function) {
        return CachedFunctionSpecialization{};
      }
      return CachedFunctionSpecialization{ast::cloneFunction(cached.function),
                                          cached.functionTypeArgs};
    });
  }

  void CompilerCache::storeStdlibFunctionSpecialization(const std::string& key,
                                                        const ast::Function& function,
                                                        const std::vector<Type>& typeArgs) {
    const std::size_t weight = specializationWeight(function, typeArgs);
    if (weight < kMinCachedStdlibFunctionSpecializationWeight) {
      return;
    }

    CachedSpecialization cached;
    cached.kind = CachedSpecialization::Kind::Function;
    cached.function = ast::cloneFunction(function);
    cached.functionTypeArgs = typeArgs;

    std::lock_guard<std::mutex> lock(mutex_);
    stdlibSpecializations_.put(key, std::move(cached));
  }

  std::optional<ast::StructDecl>
  CompilerCache::cloneStdlibStructSpecialization(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    return stdlibSpecializations_.get(key, [](const CachedSpecialization& cached) {
      if (cached.kind != CachedSpecialization::Kind::Struct) {
        return ast::StructDecl{};
      }
      return ast::cloneStructDecl(cached.structDecl);
    });
  }

  void CompilerCache::storeStdlibStructSpecialization(const std::string& key,
                                                      const ast::StructDecl& structDecl) {
    if (structDecl.fields.size() < kMinCachedStdlibStructFields) {
      return;
    }
    CachedSpecialization cached;
    cached.kind = CachedSpecialization::Kind::Struct;
    cached.structDecl = ast::cloneStructDecl(structDecl);

    std::lock_guard<std::mutex> lock(mutex_);
    stdlibSpecializations_.put(key, std::move(cached));
  }

  std::size_t CompilerCache::parsedStdlibModuleCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return parsedStdlibModules_.size();
  }

  std::size_t CompilerCache::stdlibSpecializationCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stdlibSpecializations_.size();
  }

  void CompilerCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    parsedStdlibModules_.clear();
    stdlibSpecializations_.clear();
  }

  CompilerCache& processCompilerCache() {
    static CompilerCache cache;
    return cache;
  }

  std::string stdlibRootCacheKey(const std::filesystem::path& stdlibRoot) {
    std::error_code error;
    std::filesystem::path normalized = std::filesystem::weakly_canonical(stdlibRoot, error);
    if (error) {
      error.clear();
      normalized = std::filesystem::absolute(stdlibRoot, error);
    }
    if (error) {
      normalized = stdlibRoot.lexically_normal();
    }
    return normalized.string();
  }

  std::string parsedStdlibModuleCacheKey(const std::string& stdlibRootKey,
                                         const std::string& modulePath) {
    return stdlibRootKey + "|" + modulePath;
  }

  std::string stdlibSpecializationCacheKey(std::string_view kind, const std::string& originModule,
                                           const std::string& mangledName) {
    std::ostringstream out;
    out << kind << '|' << originModule << '|' << mangledName;
    return out.str();
  }

} // namespace noria
