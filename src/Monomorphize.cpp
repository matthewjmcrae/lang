#include "noria/Monomorphize.hpp"

#include "noria/AstVisitor.hpp"
#include "noria/Diagnostic.hpp"

#include <algorithm>
#include <optional>
#include <sstream>
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
        std::vector<std::unique_ptr<ast::Expression>> arguments;
        arguments.reserve(node.arguments.size());
        for (const auto& argument : node.arguments) {
          argument->accept(*this);
          arguments.push_back(takeExpression());
        }

        std::string callee = node.callee;
        if (callee == templateName_) {
          callee = mangleSpecialization(templateName_, typeArgs_);
        }

        expression_ = std::make_unique<ast::CallExpression>(std::move(callee), std::move(arguments),
                                                            node.location);
      }

      void visit(const ast::ArrayLiteral& node) override {
        std::vector<std::unique_ptr<ast::Expression>> elements;
        elements.reserve(node.elements.size());
        for (const auto& element : node.elements) {
          element->accept(*this);
          elements.push_back(takeExpression());
        }
        expression_ = std::make_unique<ast::ArrayLiteral>(std::move(elements), node.location);
      }

      void visit(const ast::IndexExpression& node) override {
        node.base->accept(*this);
        auto base = takeExpression();
        node.index->accept(*this);
        expression_ = std::make_unique<ast::IndexExpression>(std::move(base), takeExpression(),
                                                             node.location);
      }

      void visit(const ast::StructLiteral& node) override {
        std::vector<ast::StructLiteralField> fields;
        fields.reserve(node.fields.size());
        for (const auto& field : node.fields) {
          field.value->accept(*this);
          fields.push_back(ast::StructLiteralField{field.name, takeExpression(), field.location});
        }
        std::vector<Type> typeArgs;
        typeArgs.reserve(node.typeArgs.size());
        for (const Type& typeArg : node.typeArgs) {
          typeArgs.push_back(substituteType(typeArg, substitution_));
        }
        std::string structName = node.structName;
        if (!typeArgs.empty() &&
            !containsUnboundTypeParam(Type::structType(structName, typeArgs))) {
          structName = mangleSpecialization(structName, typeArgs);
          typeArgs.clear();
        }
        expression_ = std::make_unique<ast::StructLiteral>(structName, std::move(typeArgs),
                                                           std::move(fields), node.location);
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
        node.initializer->accept(*this);
        statement_ = std::make_unique<ast::LetStatement>(
            node.name, rewriteAppliedStructType(substituteType(node.type, substitution_)),
            takeExpression(), node.location);
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
      std::string templateName_;
      std::vector<Type> typeArgs_;
      Substitution substitution_;
      std::unique_ptr<ast::Expression> expression_;
      std::unique_ptr<ast::Statement> statement_;
    };

    class CallSiteRewriteVisitor final : public ast::MutableAstVisitor {
    public:
      explicit CallSiteRewriteVisitor(const std::vector<SpecializationRequest>& requests)
          : requests_(requests) {}

      void rewriteFunction(ast::Function& function) {
        currentFunction_ = function.name;
        for (const auto& statement : function.body) {
          statement->accept(*this);
        }
      }

      void visit(ast::ReturnStatement& node) override { node.expression->accept(*this); }
      void visit(ast::LetStatement& node) override { node.initializer->accept(*this); }
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
        for (const auto& request : requests_) {
          if (locationsMatch(node.location, request.callSiteLocation) &&
              node.callee == request.templateName &&
              currentFunction_ == request.enclosingFunction) {
            node.callee = mangleSpecialization(request.templateName, request.typeArgs);
            break;
          }
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
      Substitution substitution;
      if (typeArgs.size() != templated.typeParams.size()) {
        throw CompileError("internal: struct specialization type argument count mismatch");
      }

      for (std::size_t index{}; index < templated.typeParams.size(); ++index) {
        substitution.emplace(templated.typeParams[index].name, typeArgs[index]);
      }

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

    class StructApplicationRewriteVisitor final : public ast::MutableAstVisitor {
    public:
      explicit StructApplicationRewriteVisitor(
          const std::vector<StructSpecializationRequest>& requests)
          : requests_(requests) {}

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
        node.type = rewriteAppliedStructType(node.type);
        node.initializer->accept(*this);
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

        for (const auto& request : requests_) {
          if (locationsMatch(node.location, request.useSiteLocation) &&
              node.structName == request.templateName &&
              currentFunction_ == request.enclosingFunction) {
            node.structName = mangleSpecialization(request.templateName, request.typeArgs);
            break;
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

      const std::vector<StructSpecializationRequest>& requests_;
      std::string currentFunction_;
    };

    ast::Function cloneSpecialization(const ast::Function& templated,
                                      const std::vector<Type>& typeArgs) {
      Substitution substitution;
      if (typeArgs.size() != templated.typeParams.size()) {
        throw CompileError("internal: specialization type argument count mismatch");
      }

      for (std::size_t index{}; index < templated.typeParams.size(); ++index) {
        substitution.emplace(templated.typeParams[index].name, typeArgs[index]);
      }

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

  } // namespace

  Type substitute(const Type& type, const Substitution& substitution) {
    return substituteType(type, substitution);
  }

  namespace {

    bool containsUnboundTypeParamForSpecialization(const Type& type) {
      if (type.kind == TypeKind::TypeParam) {
        return true;
      }

      if (type.kind == TypeKind::Array && type.element) {
        return containsUnboundTypeParamForSpecialization(*type.element);
      }

      if (type.kind == TypeKind::Struct) {
        for (const Type& typeArg : type.typeArgs) {
          if (containsUnboundTypeParamForSpecialization(typeArg)) {
            return true;
          }
        }
      }

      return false;
    }

    Type rewriteAppliedStructTypeForSpecialization(const Type& type) {
      if (type.kind == TypeKind::Struct && !type.typeArgs.empty()) {
        if (containsUnboundTypeParamForSpecialization(type)) {
          return type;
        }
        return Type::structType(mangleSpecialization(type.structName, type.typeArgs), {});
      }

      if (type.kind == TypeKind::Array && type.element) {
        return Type::array(rewriteAppliedStructTypeForSpecialization(*type.element));
      }

      return type;
    }

  } // namespace

  Type substituteSpecializationType(const Type& type, const Substitution& substitution) {
    return rewriteAppliedStructTypeForSpecialization(substituteType(type, substitution));
  }

  std::string mangleType(const Type& type) {
    switch (type.kind) {
    case TypeKind::I32:
      return "s.i32";
    case TypeKind::F64:
      return "s.f64";
    case TypeKind::Bool:
      return "s.bool";
    case TypeKind::Str:
      return "s.str";
    case TypeKind::RawPtr:
      return "s.rt_ptr";
    case TypeKind::Struct:
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
    case TypeKind::Array:
      return "arr." + mangleType(*type.element);
    case TypeKind::ImplTag:
      return "tag." + std::string(implementationTagName(type.implTag));
    case TypeKind::TypeParam:
      throw CompileError("internal: cannot mangle unsubstituted type parameter '" +
                         type.typeParamName + "'");
    case TypeKind::Void:
      return "s.void";
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

    const ast::Function* templated =
        findTemplateFunction(module, request.templateName, findImplTag(request.typeArgs));
    if (templated == nullptr) {
      throw CompileError(
          formatDiagnostic(request.callSiteLocation, DiagnosticStage::TypeCheck,
                           "unknown generic function '" + request.templateName + "'"));
    }

    module.functions.push_back(cloneSpecialization(*templated, request.typeArgs));
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

    const ast::StructDecl* templated = findTemplateStruct(module, request.templateName);
    if (templated == nullptr) {
      throw CompileError(formatDiagnostic(request.useSiteLocation, DiagnosticStage::TypeCheck,
                                          "unknown generic struct '" + request.templateName + "'"));
    }

    module.structs.push_back(cloneStructSpecialization(*templated, request.typeArgs));
    emittedStructs_.insert(mangledName);
    return 1;
  }

  std::size_t expandSpecializations(ast::Module& module,
                                    const std::vector<SpecializationRequest>& requests,
                                    SpecializationCache& cache) {
    std::vector<SpecializationRequest> sorted = requests;
    std::sort(sorted.begin(), sorted.end(),
              [](const SpecializationRequest& left, const SpecializationRequest& right) {
                const std::string leftName = mangleSpecialization(left.templateName, left.typeArgs);
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

    std::unordered_set<std::string> seen;
    std::size_t added = 0;

    module.functions.reserve(module.functions.size() + sorted.size());
    for (const SpecializationRequest& request : sorted) {
      const std::string mangledName = mangleSpecialization(request.templateName, request.typeArgs);
      if (!seen.insert(mangledName).second) {
        continue;
      }

      added += cache.emitFunction(module, request);
    }

    rewriteGenericCallSites(module, requests);
    return added;
  }

  std::size_t expandStructSpecializations(ast::Module& module,
                                          const std::vector<StructSpecializationRequest>& requests,
                                          SpecializationCache& cache) {
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

    std::unordered_set<std::string> seen;
    std::size_t added = 0;

    module.structs.reserve(module.structs.size() + sorted.size());
    for (const StructSpecializationRequest& request : sorted) {
      const std::string mangledName = mangleSpecialization(request.templateName, request.typeArgs);
      if (!seen.insert(mangledName).second) {
        continue;
      }

      added += cache.emitStruct(module, request);
    }

    StructApplicationRewriteVisitor rewriter(requests);
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
    CallSiteRewriteVisitor rewriter(requests);
    for (auto& function : module.functions) {
      rewriter.rewriteFunction(function);
    }
  }

} // namespace noria
