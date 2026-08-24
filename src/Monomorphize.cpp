#include "noria/Monomorphize.hpp"

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

namespace noria {

  namespace {

    bool locationsMatch(const SourceLocation& left, const SourceLocation& right) {
      if (left.file != right.file) {
        return false;
      }
      return left.line == right.line && left.column == right.column;
    }

    std::optional<ImplementationTag> findImplTag(const std::vector<Type>& typeArgs) {
      for (const Type& typeArg : typeArgs) {
        if (typeArg.kind == TypeKind::ImplTag) {
          return typeArg.implTag;
        }
      }
      return std::nullopt;
    }

    bool locationLess(const SourceLocation& left, const SourceLocation& right) {
      if (left.file != right.file) {
        if (left.file.empty()) {
          return !right.file.empty();
        }
        if (right.file.empty()) {
          return false;
        }
        return left.file < right.file;
      }
      if (left.line != right.line) {
        return left.line < right.line;
      }
      return left.column < right.column;
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

    std::string parentSpecializationMangled(std::string_view enclosingFunction) {
      if (enclosingFunction.find('$') == std::string_view::npos) {
        return {};
      }
      return std::string(enclosingFunction);
    }

    struct PendingSpecializations {
      std::vector<StructSpecializationRequest> structs;
      std::vector<SpecializationRequest> functions;

      bool empty() const { return structs.empty() && functions.empty(); }
    };

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
        const std::string childMangled =
            mangleSpecialization(request.templateName, request.typeArgs);
        if (!linked.insert(childMangled).second) {
          continue;
        }
        cache.link(childMangled, parentSpecializationMangled(request.enclosingFunction),
                   request.useSiteLocation);
      }
      for (const SpecializationRequest& request : functionRequests) {
        const std::string childMangled =
            mangleSpecialization(request.templateName, request.typeArgs);
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

    Type substituteType(const Type& type, const Substitution& substitution) {
      if (type.kind == TypeKind::TypeParam) {
        const auto bound = substitution.find(type.typeParamName);
        if (bound == substitution.end()) {
          throw CompileError("internal: unbound type parameter '" + type.typeParamName + "'");
        }
        return bound->second;
      }

      if (type.kind == TypeKind::Array) {
        if (!type.element) {
          return type;
        }
        return Type::array(substituteType(*type.element, substitution));
      }

      if (type.kind == TypeKind::Struct) {
        if (type.typeArgs.empty()) {
          return type;
        }
        std::vector<Type> substitutedArgs;
        substitutedArgs.reserve(type.typeArgs.size());
        for (const Type& typeArg : type.typeArgs) {
          substitutedArgs.push_back(substituteType(typeArg, substitution));
        }
        return Type::structType(type.structName, std::move(substitutedArgs));
      }

      return type;
    }

    bool containsUnboundTypeParam(const Type& type) {
      if (type.kind == TypeKind::TypeParam) {
        return true;
      }

      if (type.kind == TypeKind::Array && type.element) {
        return containsUnboundTypeParam(*type.element);
      }

      if (type.kind == TypeKind::Struct) {
        for (const Type& typeArg : type.typeArgs) {
          if (containsUnboundTypeParam(typeArg)) {
            return true;
          }
        }
      }

      return false;
    }

    Type rewriteAppliedStructType(const Type& type) {
      if (type.kind == TypeKind::Struct && !type.typeArgs.empty()) {
        if (containsUnboundTypeParam(type)) {
          return type;
        }
        return Type::structType(mangleSpecialization(type.structName, type.typeArgs), {});
      }

      if (type.kind == TypeKind::Array && type.element) {
        return Type::array(rewriteAppliedStructType(*type.element));
      }

      return type;
    }

    Substitution bindTypeParameters(const std::vector<ast::TypeParameter>& typeParams,
                                    const std::vector<Type>& typeArgs, std::string_view context) {
      if (typeArgs.size() != typeParams.size()) {
        throw CompileError("internal: " + std::string(context) + " type argument count mismatch");
      }

      Substitution substitution;
      for (std::size_t index{}; index < typeParams.size(); ++index) {
        substitution.emplace(typeParams[index].name, typeArgs[index]);
      }
      return substitution;
    }

    // Clone a generic template body while substituting type parameters and self-recursive calls.
    class CloneVisitor final : public ast::AstVisitor {
    public:
      CloneVisitor(std::string templateName, std::vector<Type> typeArgs, Substitution substitution)
          : templateName_(std::move(templateName)), typeArgs_(std::move(typeArgs)),
            substitution_(std::move(substitution)) {}

      std::unique_ptr<ast::Expression> takeExpression() { return std::move(expression_); }
      std::unique_ptr<ast::Statement> takeStatement() { return std::move(statement_); }

      void visit(const ast::IntegerLiteral& node) override {
        expression_ = std::make_unique<ast::IntegerLiteral>(node.value, node.location);
      }

      void visit(const ast::FloatLiteral& node) override {
        expression_ = std::make_unique<ast::FloatLiteral>(node.value, node.location);
      }

      void visit(const ast::StringLiteral& node) override {
        expression_ = std::make_unique<ast::StringLiteral>(node.value, node.location);
      }

      void visit(const ast::BoolLiteral& node) override {
        expression_ = std::make_unique<ast::BoolLiteral>(node.value, node.location);
      }

      void visit(const ast::IdentifierExpression& node) override {
        expression_ = std::make_unique<ast::IdentifierExpression>(node.name, node.location);
      }

      void visit(const ast::UnaryExpression& node) override {
        node.operand->accept(*this);
        expression_ =
            std::make_unique<ast::UnaryExpression>(node.op, takeExpression(), node.location);
      }

      void visit(const ast::CastExpression& node) override {
        node.expression->accept(*this);
        expression_ = std::make_unique<ast::CastExpression>(
            takeExpression(), substituteType(node.targetType, substitution_), node.location);
      }

      void visit(const ast::BinaryExpression& node) override {
        node.left->accept(*this);
        auto left = takeExpression();
        node.right->accept(*this);
        expression_ = std::make_unique<ast::BinaryExpression>(node.op, std::move(left),
                                                              takeExpression(), node.location);
      }

      void visit(const ast::CallExpression& node) override {
        expression_ = std::make_unique<ast::CallExpression>(
            specializedCalleeName(node.callee), cloneExpressions(node.arguments), node.location);
      }

      void visit(const ast::ArrayLiteral& node) override {
        expression_ =
            std::make_unique<ast::ArrayLiteral>(cloneExpressions(node.elements), node.location);
      }

      void visit(const ast::IndexExpression& node) override {
        node.base->accept(*this);
        auto base = takeExpression();
        node.index->accept(*this);
        expression_ = std::make_unique<ast::IndexExpression>(std::move(base), takeExpression(),
                                                             node.location);
      }

      void visit(const ast::StructLiteral& node) override {
        std::vector<Type> typeArgs = substituteTypeArguments(node.typeArgs);
        expression_ = std::make_unique<ast::StructLiteral>(
            node.structName, std::move(typeArgs), cloneStructFields(node.fields), node.location);
      }

      void visit(const ast::FieldAccessExpression& node) override {
        node.base->accept(*this);
        expression_ = std::make_unique<ast::FieldAccessExpression>(takeExpression(), node.fieldName,
                                                                   node.location);
      }

      void visit(const ast::ReturnStatement& node) override {
        node.expression->accept(*this);
        statement_ = std::make_unique<ast::ReturnStatement>(takeExpression(), node.location);
      }

      void visit(const ast::LetStatement& node) override {
        std::optional<Type> declaredType;
        if (node.declaredType) {
          declaredType = rewriteAppliedStructType(substituteType(*node.declaredType, substitution_));
        }
        std::unique_ptr<ast::Expression> initializer;
        if (node.initializer) {
          node.initializer->accept(*this);
          initializer = takeExpression();
        }
        statement_ = std::make_unique<ast::LetStatement>(
            node.name, std::move(declaredType), std::move(initializer), node.location);
      }

      void visit(const ast::IfStatement& node) override {
        node.condition->accept(*this);
        auto condition = takeExpression();
        statement_ = std::make_unique<ast::IfStatement>(
            std::move(condition), cloneStatements(node.thenBranch),
            cloneStatements(node.elseBranch), node.location);
      }

      void visit(const ast::WhileStatement& node) override {
        node.condition->accept(*this);
        auto condition = takeExpression();
        statement_ = std::make_unique<ast::WhileStatement>(
            std::move(condition), cloneStatements(node.body), node.location);
      }

      void visit(const ast::AssignmentStatement& node) override {
        node.lhs->accept(*this);
        auto lhs = takeExpression();
        node.rhs->accept(*this);
        statement_ = std::make_unique<ast::AssignmentStatement>(std::move(lhs), takeExpression(),
                                                                node.location);
      }

      void visit(const ast::ExpressionStatement& node) override {
        node.expression->accept(*this);
        statement_ = std::make_unique<ast::ExpressionStatement>(takeExpression(), node.location);
      }

      std::vector<std::unique_ptr<ast::Statement>>
      cloneStatements(const std::vector<std::unique_ptr<ast::Statement>>& statements) {
        std::vector<std::unique_ptr<ast::Statement>> cloned;
        cloned.reserve(statements.size());
        for (const auto& statement : statements) {
          statement->accept(*this);
          cloned.push_back(takeStatement());
        }
        return cloned;
      }

    private:
      std::vector<std::unique_ptr<ast::Expression>>
      cloneExpressions(const std::vector<std::unique_ptr<ast::Expression>>& expressions) {
        std::vector<std::unique_ptr<ast::Expression>> cloned;
        cloned.reserve(expressions.size());
        for (const auto& expression : expressions) {
          expression->accept(*this);
          cloned.push_back(takeExpression());
        }
        return cloned;
      }

      std::vector<ast::StructLiteralField>
      cloneStructFields(const std::vector<ast::StructLiteralField>& fields) {
        std::vector<ast::StructLiteralField> cloned;
        cloned.reserve(fields.size());
        for (const auto& field : fields) {
          field.value->accept(*this);
          cloned.push_back(ast::StructLiteralField{field.name, takeExpression(), field.location});
        }
        return cloned;
      }

      std::vector<Type> substituteTypeArguments(const std::vector<Type>& typeArgs) {
        std::vector<Type> substituted;
        substituted.reserve(typeArgs.size());
        for (const Type& typeArg : typeArgs) {
          substituted.push_back(substituteType(typeArg, substitution_));
        }
        return substituted;
      }

      std::string specializedCalleeName(std::string_view callee) const {
        if (callee == templateName_) {
          return mangleSpecialization(templateName_, typeArgs_);
        }
        return std::string(callee);
      }

      std::string templateName_;
      std::vector<Type> typeArgs_;
      Substitution substitution_;
      std::unique_ptr<ast::Expression> expression_;
      std::unique_ptr<ast::Statement> statement_;
    };

    // Rewrite call sites that requested concrete function specializations during type checking.
    class CallSiteMutator final : public ast::AstMutator {
    public:
      explicit CallSiteMutator(const std::vector<SpecializationRequest>& requests)
          : requests_(requests), matched_(requests.size(), false) {}

      void rewriteFunction(ast::Function& function) {
        currentFunction_ = function.name;
        for (const auto& statement : function.body) {
          statement->accept(*this);
        }
      }

      bool allMatched() const {
        return std::all_of(matched_.begin(), matched_.end(), [](bool matched) {
          return matched;
        });
      }

      const std::vector<bool>& matched() const { return matched_; }

      void visit(ast::ReturnStatement& node) override { node.expression->accept(*this); }
      void visit(ast::LetStatement& node) override {
        if (node.initializer) {
          node.initializer->accept(*this);
        }
      }
      void visit(ast::IfStatement& node) override {
        node.condition->accept(*this);
        visitStatements(node.thenBranch);
        visitStatements(node.elseBranch);
      }
      void visit(ast::WhileStatement& node) override {
        node.condition->accept(*this);
        visitStatements(node.body);
      }
      void visit(ast::AssignmentStatement& node) override {
        node.lhs->accept(*this);
        node.rhs->accept(*this);
      }
      void visit(ast::ExpressionStatement& node) override { node.expression->accept(*this); }

      void visit(ast::UnaryExpression& node) override { node.operand->accept(*this); }
      void visit(ast::CastExpression& node) override { node.expression->accept(*this); }
      void visit(ast::BinaryExpression& node) override {
        node.left->accept(*this);
        node.right->accept(*this);
      }
      void visit(ast::CallExpression& node) override {
        const std::string originalCallee = node.callee;
        std::optional<std::string> selectedCallee;
        for (std::size_t index{}; index < requests_.size(); ++index) {
          const SpecializationRequest& request = requests_[index];
          if (locationsMatch(node.location, request.callSiteLocation) &&
              originalCallee == request.templateName &&
              currentFunction_ == request.enclosingFunction) {
            const std::string mangled =
                mangleSpecialization(request.templateName, request.typeArgs);
            if (selectedCallee && *selectedCallee != mangled) {
              throw CompileError("monomorphize: internal error: ambiguous generic call rewrite");
            }
            selectedCallee = mangled;
            matched_[index] = true;
          }
        }
        if (selectedCallee) {
          node.callee = std::move(*selectedCallee);
        }
        for (const auto& argument : node.arguments) {
          argument->accept(*this);
        }
      }
      void visit(ast::ArrayLiteral& node) override {
        for (const auto& element : node.elements) {
          element->accept(*this);
        }
      }
      void visit(ast::IndexExpression& node) override {
        node.base->accept(*this);
        node.index->accept(*this);
      }
      void visit(ast::StructLiteral& node) override {
        for (const auto& field : node.fields) {
          field.value->accept(*this);
        }
      }
      void visit(ast::FieldAccessExpression& node) override { node.base->accept(*this); }

      void visit(ast::IntegerLiteral&) override {}
      void visit(ast::FloatLiteral&) override {}
      void visit(ast::StringLiteral&) override {}
      void visit(ast::BoolLiteral&) override {}
      void visit(ast::IdentifierExpression&) override {}

    private:
      void visitStatements(const std::vector<std::unique_ptr<ast::Statement>>& statements) {
        for (const auto& statement : statements) {
          statement->accept(*this);
        }
      }

      const std::vector<SpecializationRequest>& requests_;
      std::vector<bool> matched_;
      std::string currentFunction_;
    };

    const ast::Function* findTemplateFunction(const ast::Module& module, std::string_view name,
                                              std::optional<ImplementationTag> implTag) {
      for (const auto& function : module.functions) {
        if (function.name != name || function.typeParams.empty()) {
          continue;
        }

        if (implTag) {
          if (function.implTag && *function.implTag == *implTag) {
            return &function;
          }
          continue;
        }

        if (!function.implTag) {
          return &function;
        }
      }
      return nullptr;
    }

    const ast::StructDecl* findTemplateStruct(const ast::Module& module, std::string_view name) {
      for (const auto& structDecl : module.structs) {
        if (structDecl.name == name && !structDecl.typeParams.empty()) {
          return &structDecl;
        }
      }
      return nullptr;
    }

    ast::StructDecl cloneStructSpecialization(const ast::StructDecl& templated,
                                              const std::vector<Type>& typeArgs) {
      const Substitution substitution =
          bindTypeParameters(templated.typeParams, typeArgs, "struct specialization");

      ast::StructDecl specialized;
      specialized.name = mangleSpecialization(templated.name, typeArgs);
      specialized.location = templated.location;
      specialized.typeParams = {};
      specialized.fields.reserve(templated.fields.size());
      for (const auto& field : templated.fields) {
        specialized.fields.push_back(ast::StructField{field.name,
                                                      substituteType(field.type, substitution),
                                                      field.location, field.visibility});
      }
      return specialized;
    }

    // Rewrite concrete generic struct applications after their specialized structs are emitted.
    class StructApplicationMutator final : public ast::AstMutator {
    public:
      explicit StructApplicationMutator(const std::vector<StructSpecializationRequest>& requests)
          : requestsByFunction_(groupByEnclosingFunction(requests)) {}

      void rewriteFunction(ast::Function& function) {
        currentFunction_ = function.name;
        for (const auto& statement : function.body) {
          statement->accept(*this);
        }
      }

      void rewriteModule(ast::Module& module) {
        for (auto& structDecl : module.structs) {
          if (!structDecl.typeParams.empty()) {
            continue;
          }

          for (auto& field : structDecl.fields) {
            field.type = rewriteAppliedStructType(field.type);
          }
        }

        for (auto& function : module.functions) {
          if (!function.typeParams.empty()) {
            continue;
          }

          function.returnType = rewriteAppliedStructType(function.returnType);
          for (auto& parameter : function.parameters) {
            parameter.type = rewriteAppliedStructType(parameter.type);
          }
          rewriteFunction(function);
        }
      }

      void visit(ast::ReturnStatement& node) override { node.expression->accept(*this); }
      void visit(ast::LetStatement& node) override {
        if (node.declaredType) {
          node.declaredType = rewriteAppliedStructType(*node.declaredType);
        }
        if (node.initializer) {
          node.initializer->accept(*this);
        }
      }
      void visit(ast::IfStatement& node) override {
        node.condition->accept(*this);
        visitStatements(node.thenBranch);
        visitStatements(node.elseBranch);
      }
      void visit(ast::WhileStatement& node) override {
        node.condition->accept(*this);
        visitStatements(node.body);
      }
      void visit(ast::AssignmentStatement& node) override {
        node.lhs->accept(*this);
        node.rhs->accept(*this);
      }
      void visit(ast::ExpressionStatement& node) override { node.expression->accept(*this); }

      void visit(ast::UnaryExpression& node) override { node.operand->accept(*this); }
      void visit(ast::CastExpression& node) override {
        node.targetType = rewriteAppliedStructType(node.targetType);
        node.expression->accept(*this);
      }
      void visit(ast::BinaryExpression& node) override {
        node.left->accept(*this);
        node.right->accept(*this);
      }
      void visit(ast::CallExpression& node) override {
        for (const auto& argument : node.arguments) {
          argument->accept(*this);
        }
      }
      void visit(ast::ArrayLiteral& node) override {
        for (const auto& element : node.elements) {
          element->accept(*this);
        }
      }
      void visit(ast::IndexExpression& node) override {
        node.base->accept(*this);
        node.index->accept(*this);
      }
      void visit(ast::StructLiteral& node) override {
        if (!node.typeArgs.empty()) {
          if (!containsUnboundTypeParam(Type::structType(node.structName, node.typeArgs))) {
            node.structName = mangleSpecialization(node.structName, node.typeArgs);
            node.typeArgs.clear();
          }
          for (const auto& field : node.fields) {
            field.value->accept(*this);
          }
          return;
        }

        const auto requests = requestsByFunction_.find(currentFunction_);
        if (requests != requestsByFunction_.end()) {
          for (const auto& request : requests->second) {
            if (locationsMatch(node.location, request.useSiteLocation) &&
                node.structName == request.templateName) {
              node.structName = mangleSpecialization(request.templateName, request.typeArgs);
              break;
            }
          }
        }

        for (const auto& field : node.fields) {
          field.value->accept(*this);
        }
      }
      void visit(ast::FieldAccessExpression& node) override { node.base->accept(*this); }

      void visit(ast::IntegerLiteral&) override {}
      void visit(ast::FloatLiteral&) override {}
      void visit(ast::StringLiteral&) override {}
      void visit(ast::BoolLiteral&) override {}
      void visit(ast::IdentifierExpression&) override {}

    private:
      void visitStatements(const std::vector<std::unique_ptr<ast::Statement>>& statements) {
        for (const auto& statement : statements) {
          statement->accept(*this);
        }
      }

      static std::unordered_map<std::string, std::vector<StructSpecializationRequest>>
      groupByEnclosingFunction(const std::vector<StructSpecializationRequest>& requests) {
        std::unordered_map<std::string, std::vector<StructSpecializationRequest>> grouped;
        for (const StructSpecializationRequest& request : requests) {
          grouped[request.enclosingFunction].push_back(request);
        }
        return grouped;
      }

      std::unordered_map<std::string, std::vector<StructSpecializationRequest>> requestsByFunction_;
      std::string currentFunction_;
    };

    ast::Function cloneSpecialization(const ast::Function& templated,
                                      const std::vector<Type>& typeArgs) {
      const Substitution substitution =
          bindTypeParameters(templated.typeParams, typeArgs, "specialization");

      ast::Function specialized;
      specialized.name = mangleSpecialization(templated.name, typeArgs);
      specialized.returnType =
          rewriteAppliedStructType(substituteType(templated.returnType, substitution));
      specialized.location = templated.location;
      specialized.typeParams = {};

      specialized.parameters.reserve(templated.parameters.size());
      for (const auto& parameter : templated.parameters) {
        specialized.parameters.push_back(ast::Parameter{
            parameter.name, rewriteAppliedStructType(substituteType(parameter.type, substitution)),
            parameter.location});
      }

      CloneVisitor cloner(templated.name, typeArgs, substitution);
      specialized.body = cloner.cloneStatements(templated.body);
      return specialized;
    }

    std::vector<SpecializationRequest>
    sortedFunctionRequests(const std::vector<SpecializationRequest>& requests) {
      std::vector<SpecializationRequest> sorted = requests;
      std::sort(sorted.begin(), sorted.end(),
                [](const SpecializationRequest& left, const SpecializationRequest& right) {
                  const std::string leftName =
                      mangleSpecialization(left.templateName, left.typeArgs);
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
        const std::string mangledName =
            mangleSpecialization(request.templateName, request.typeArgs);
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
        const std::string mangledName =
            mangleSpecialization(request.templateName, request.typeArgs);
        if (!seen.insert(mangledName).second) {
          continue;
        }

        added += cache.emitStruct(module, request);
      }
      return added;
    }

    std::string locationKey(const SourceLocation& location) {
      std::ostringstream out;
      out << location.file << ':' << location.line << ':' << location.column;
      return out.str();
    }

    std::vector<SpecializationRequest>
    dedupeFunctionRewriteRequests(const std::vector<SpecializationRequest>& requests) {
      std::vector<SpecializationRequest> unique;
      std::unordered_set<std::string> seen;
      unique.reserve(requests.size());
      for (const SpecializationRequest& request : requests) {
        const std::string key =
            request.enclosingFunction + "|" + request.templateName + "|" +
            mangleSpecialization(request.templateName, request.typeArgs) + "|" +
            locationKey(request.callSiteLocation);
        if (!seen.insert(key).second) {
          continue;
        }
        unique.push_back(request);
      }
      return unique;
    }

    std::unordered_map<std::string, std::size_t>
    concreteFunctionIndexByName(const ast::Module& module) {
      std::unordered_map<std::string, std::size_t> indexes;
      for (std::size_t index{}; index < module.functions.size(); ++index) {
        const ast::Function& function = module.functions[index];
        if (function.typeParams.empty()) {
          indexes.emplace(function.name, index);
        }
      }
      return indexes;
    }

    std::unordered_map<std::string, std::vector<SpecializationRequest>>
    groupFunctionRequestsByEnclosingFunction(const std::vector<SpecializationRequest>& requests) {
      std::unordered_map<std::string, std::vector<SpecializationRequest>> grouped;
      for (const SpecializationRequest& request : requests) {
        if (request.enclosingFunction.empty()) {
          throw CompileError(
              "monomorphize: internal error: generic function request has no enclosing function");
        }
        grouped[request.enclosingFunction].push_back(request);
      }
      return grouped;
    }

    [[noreturn]] void throwMissingRewrite(const SpecializationRequest& request) {
      throw CompileError("monomorphize: internal error: recorded call to generic function '" +
                         request.templateName + "' in '" + request.enclosingFunction +
                         "' at " + locationKey(request.callSiteLocation) + " was not found");
    }

    void rewriteTargetedGenericCallSites(ast::Module& module,
                                         const std::vector<SpecializationRequest>& requests) {
      const std::vector<SpecializationRequest> unique = dedupeFunctionRewriteRequests(requests);
      const auto grouped = groupFunctionRequestsByEnclosingFunction(unique);
      const auto functionIndexes = concreteFunctionIndexByName(module);

      for (const auto& [functionName, functionRequests] : grouped) {
        const auto functionIndex = functionIndexes.find(functionName);
        if (functionIndex == functionIndexes.end()) {
          throwMissingRewrite(functionRequests.front());
        }

        CallSiteMutator rewriter(functionRequests);
        rewriter.rewriteFunction(module.functions.at(functionIndex->second));
        if (!rewriter.allMatched()) {
          const std::vector<bool>& matched = rewriter.matched();
          for (std::size_t index{}; index < matched.size(); ++index) {
            if (!matched[index]) {
              throwMissingRewrite(functionRequests[index]);
            }
          }
        }
      }
    }

    std::size_t emitPendingSpecializations(ast::Module& module, SpecializationCache& cache,
                                           const PendingSpecializations& pending) {
      std::size_t added = 0;
      if (!pending.structs.empty()) {
        const std::vector<StructSpecializationRequest> sorted =
            sortedStructRequests(pending.structs);
        added += emitUniqueStructSpecializations(module, sorted, cache);
      }
      if (!pending.functions.empty()) {
        const std::vector<SpecializationRequest> sorted =
            sortedFunctionRequests(pending.functions);
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
      }
      for (const SpecializationRequest& request : pending.functions) {
        lastSpecializationLocation = request.callSiteLocation;
        propagateFunctionSpecializationOrigin(symbolOrigins, request.templateName,
                                             request.typeArgs);
        checker.registerFunctionSpecialization(
            mangleSpecialization(request.templateName, request.typeArgs), request.typeArgs);
      }
    }

    void rewriteFinalSpecializations(ast::Module& module,
                                     const PendingSpecializations& accumulated) {
      if (!accumulated.functions.empty()) {
        rewriteTargetedGenericCallSites(module, accumulated.functions);
      }
      if (!accumulated.structs.empty()) {
        StructApplicationMutator rewriter(accumulated.structs);
        rewriter.rewriteModule(module);
      }
    }

  } // namespace

  Type substitute(const Type& type, const Substitution& substitution) {
    return substituteType(type, substitution);
  }

  Type substituteSpecializationType(const Type& type, const Substitution& substitution) {
    return rewriteAppliedStructType(substituteType(type, substitution));
  }

  std::string mangleType(const Type& type) {
    if (type.kind == TypeKind::Struct) {
      if (type.typeArgs.empty()) {
        return "st." + type.structName;
      }
      {
        std::ostringstream out;
        out << "st." << type.structName;
        for (const Type& typeArg : type.typeArgs) {
          out << "$" << mangleType(typeArg);
        }
        return out.str();
      }
    }

    if (type.kind == TypeKind::Array) {
      return "arr." + mangleType(*type.element);
    }

    if (type.kind == TypeKind::ImplTag) {
      return "tag." + std::string(implementationTagName(type.implTag));
    }

    if (type.kind == TypeKind::TypeParam) {
      throw CompileError("internal: cannot mangle unsubstituted type parameter '" +
                         type.typeParamName + "'");
    }

    if (const TypeKindInfo* info = typeKindInfo(type.kind); info && !info->mangleAtom.empty()) {
      return std::string(info->mangleAtom);
    }
    return "unknown";
  }

  std::string mangleSpecialization(std::string_view templateName,
                                   const std::vector<Type>& typeArgs) {
    std::ostringstream out;
    out << templateName;
    for (const Type& typeArg : typeArgs) {
      out << '$' << mangleType(typeArg);
    }
    return out.str();
  }

  SpecializationCache::SpecializationCache(CompilerCache* compilerCache,
                                           SymbolOrigins* symbolOrigins)
      : compilerCache_(compilerCache), symbolOrigins_(symbolOrigins) {}

  void SpecializationCache::seedFromModule(const ast::Module& module) {
    for (const auto& function : module.functions) {
      if (function.typeParams.empty()) {
        emittedFunctions_.insert(function.name);
      }
    }

    for (const auto& structDecl : module.structs) {
      if (structDecl.typeParams.empty()) {
        emittedStructs_.insert(structDecl.name);
      }
    }
  }

  bool SpecializationCache::hasFunction(std::string_view mangledName) const {
    return emittedFunctions_.contains(std::string(mangledName));
  }

  bool SpecializationCache::hasStruct(std::string_view mangledName) const {
    return emittedStructs_.contains(std::string(mangledName));
  }

  void SpecializationCache::throwCycle(SourceLocation location, std::string_view childMangled,
                                       std::string_view parentMangled) const {
    std::ostringstream out;
    out << "recursive generic specialization: " << childMangled;
    std::string current(parentMangled);
    while (current != childMangled) {
      out << " -> " << current;
      const auto parent = dependencyParent_.find(current);
      if (parent == dependencyParent_.end()) {
        break;
      }
      current = parent->second;
    }
    out << " -> " << childMangled;
    throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck, out.str()));
  }

  void SpecializationCache::link(std::string_view childMangled, std::string_view parentMangled,
                                 SourceLocation location) {
    const std::string child(childMangled);
    if (hasFunction(child) || hasStruct(child)) {
      return;
    }

    if (parentMangled.empty()) {
      return;
    }

    std::string current(parentMangled);
    while (true) {
      if (current == child) {
        throwCycle(location, child, parentMangled);
      }
      const auto parent = dependencyParent_.find(current);
      if (parent == dependencyParent_.end()) {
        break;
      }
      current = parent->second;
    }

    dependencyParent_.emplace(child, std::string(parentMangled));
  }

  void SpecializationCache::clearLinks() {
    dependencyParent_.clear();
  }

  std::size_t SpecializationCache::emitFunction(ast::Module& module,
                                                const SpecializationRequest& request) {
    const std::string mangledName = mangleSpecialization(request.templateName, request.typeArgs);
    if (hasFunction(mangledName)) {
      return 0;
    }

    const std::optional<std::string> cacheKey =
        stdlibFunctionSpecializationCacheKey(symbolOrigins_, request, mangledName);
    if (compilerCache_ != nullptr && cacheKey.has_value()) {
      if (std::optional<CachedFunctionSpecialization> cached =
              compilerCache_->cloneStdlibFunctionSpecialization(*cacheKey)) {
        module.functions.push_back(std::move(cached->function));
        emittedFunctions_.insert(mangledName);
        functionSpecializationTypeArgs_.emplace(mangledName, std::move(cached->typeArgs));
        return 1;
      }
    }

    const ast::Function* templated =
        findTemplateFunction(module, request.templateName, findImplTag(request.typeArgs));
    if (templated == nullptr) {
      throw CompileError(
          formatDiagnostic(request.callSiteLocation, DiagnosticStage::TypeCheck,
                           "unknown generic function '" + request.templateName + "'"));
    }

    ast::Function specialized = cloneSpecialization(*templated, request.typeArgs);
    if (compilerCache_ != nullptr && cacheKey.has_value()) {
      compilerCache_->storeStdlibFunctionSpecialization(*cacheKey, specialized, request.typeArgs);
    }
    module.functions.push_back(std::move(specialized));
    emittedFunctions_.insert(mangledName);
    functionSpecializationTypeArgs_.emplace(mangledName, request.typeArgs);
    return 1;
  }

  std::size_t SpecializationCache::emitStruct(ast::Module& module,
                                              const StructSpecializationRequest& request) {
    const std::string mangledName = mangleSpecialization(request.templateName, request.typeArgs);
    if (hasStruct(mangledName)) {
      return 0;
    }

    const std::optional<std::string> cacheKey =
        stdlibStructSpecializationCacheKey(symbolOrigins_, request, mangledName);
    if (compilerCache_ != nullptr && cacheKey.has_value()) {
      if (std::optional<ast::StructDecl> cached =
              compilerCache_->cloneStdlibStructSpecialization(*cacheKey)) {
        module.structs.push_back(std::move(*cached));
        emittedStructs_.insert(mangledName);
        return 1;
      }
    }

    const ast::StructDecl* templated = findTemplateStruct(module, request.templateName);
    if (templated == nullptr) {
      throw CompileError(formatDiagnostic(request.useSiteLocation, DiagnosticStage::TypeCheck,
                                          "unknown generic struct '" + request.templateName + "'"));
    }

    ast::StructDecl specialized = cloneStructSpecialization(*templated, request.typeArgs);
    if (compilerCache_ != nullptr && cacheKey.has_value()) {
      compilerCache_->storeStdlibStructSpecialization(*cacheKey, specialized);
    }
    module.structs.push_back(std::move(specialized));
    emittedStructs_.insert(mangledName);
    return 1;
  }

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
    StructApplicationMutator rewriter(requests);
    rewriter.rewriteModule(module);

    return added;
  }

  void stripGenericTemplates(ast::Module& module) {
    module.structs.erase(std::remove_if(module.structs.begin(), module.structs.end(),
                                        [](const ast::StructDecl& structDecl) {
                                          return !structDecl.typeParams.empty();
                                        }),
                         module.structs.end());
  }

  void rewriteGenericCallSites(ast::Module& module,
                               const std::vector<SpecializationRequest>& requests) {
    CallSiteMutator rewriter(requests);
    for (auto& function : module.functions) {
      rewriter.rewriteFunction(function);
    }
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
    return MonomorphizationResult{cache.functionSpecializationTypeArgs()};
  }

} // namespace noria
