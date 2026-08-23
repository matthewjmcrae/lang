#include "noria/Monomorphize.hpp"

#include "noria/AstVisitor.hpp"
#include "noria/Diagnostic.hpp"

#include <algorithm>
#include <sstream>
#include <unordered_set>

namespace noria {

  namespace {

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
        expression_ = std::make_unique<ast::StructLiteral>(node.structName, std::move(typeArgs),
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
            node.name, substituteType(node.type, substitution_), takeExpression(), node.location);
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

    class CallSiteRewriteVisitor final : public ast::AstVisitor {
    public:
      explicit CallSiteRewriteVisitor(const std::vector<SpecializationRequest>& requests)
          : requests_(requests) {}

      void rewriteFunction(ast::Function& function) {
        currentFunction_ = function.name;
        for (const auto& statement : function.body) {
          statement->accept(*this);
        }
      }

      void visit(const ast::ReturnStatement& node) override { node.expression->accept(*this); }
      void visit(const ast::LetStatement& node) override { node.initializer->accept(*this); }
      void visit(const ast::IfStatement& node) override {
        node.condition->accept(*this);
        visitStatements(node.thenBranch);
        visitStatements(node.elseBranch);
      }
      void visit(const ast::WhileStatement& node) override {
        node.condition->accept(*this);
        visitStatements(node.body);
      }
      void visit(const ast::AssignmentStatement& node) override {
        node.lhs->accept(*this);
        node.rhs->accept(*this);
      }
      void visit(const ast::ExpressionStatement& node) override { node.expression->accept(*this); }

      void visit(const ast::UnaryExpression& node) override { node.operand->accept(*this); }
      void visit(const ast::CastExpression& node) override { node.expression->accept(*this); }
      void visit(const ast::BinaryExpression& node) override {
        node.left->accept(*this);
        node.right->accept(*this);
      }
      void visit(const ast::CallExpression& node) override {
        for (const auto& request : requests_) {
          if (node.location.line == request.callSiteLocation.line &&
              node.location.column == request.callSiteLocation.column &&
              node.callee == request.templateName &&
              currentFunction_ == request.enclosingFunction) {
            const_cast<ast::CallExpression&>(node).callee =
                mangleSpecialization(request.templateName, request.typeArgs);
            break;
          }
        }
        for (const auto& argument : node.arguments) {
          argument->accept(*this);
        }
      }
      void visit(const ast::ArrayLiteral& node) override {
        for (const auto& element : node.elements) {
          element->accept(*this);
        }
      }
      void visit(const ast::IndexExpression& node) override {
        node.base->accept(*this);
        node.index->accept(*this);
      }
      void visit(const ast::StructLiteral& node) override {
        for (const auto& field : node.fields) {
          field.value->accept(*this);
        }
      }
      void visit(const ast::FieldAccessExpression& node) override { node.base->accept(*this); }

      void visit(const ast::IntegerLiteral&) override {}
      void visit(const ast::FloatLiteral&) override {}
      void visit(const ast::StringLiteral&) override {}
      void visit(const ast::BoolLiteral&) override {}
      void visit(const ast::IdentifierExpression&) override {}

    private:
      void visitStatements(const std::vector<std::unique_ptr<ast::Statement>>& statements) {
        for (const auto& statement : statements) {
          statement->accept(*this);
        }
      }

      const std::vector<SpecializationRequest>& requests_;
      std::string currentFunction_;
    };

    const ast::Function* findTemplateFunction(const ast::Module& module, std::string_view name) {
      for (const auto& function : module.functions) {
        if (function.name == name && !function.typeParams.empty()) {
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

    bool hasFunctionSpecialization(const ast::Module& module, std::string_view mangledName) {
      for (const auto& function : module.functions) {
        if (function.name == mangledName && function.typeParams.empty()) {
          return true;
        }
      }
      return false;
    }

    bool hasStructSpecialization(const ast::Module& module, std::string_view mangledName) {
      for (const auto& structDecl : module.structs) {
        if (structDecl.name == mangledName && structDecl.typeParams.empty()) {
          return true;
        }
      }
      return false;
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
        specialized.fields.push_back(
            ast::StructField{field.name, substituteType(field.type, substitution), field.location});
      }
      return specialized;
    }

    Type rewriteAppliedStructType(const Type& type) {
      if (type.kind == TypeKind::Struct && !type.typeArgs.empty()) {
        return Type::structType(mangleSpecialization(type.structName, type.typeArgs), {});
      }

      if (type.kind == TypeKind::Array && type.element) {
        return Type::array(rewriteAppliedStructType(*type.element));
      }

      return type;
    }

    class StructApplicationRewriteVisitor final : public ast::AstVisitor {
    public:
      explicit StructApplicationRewriteVisitor(
          const std::vector<StructSpecializationRequest>& requests)
          : requests_(requests) {}

      void rewriteModule(ast::Module& module) {
        for (auto& structDecl : module.structs) {
          for (auto& field : structDecl.fields) {
            field.type = rewriteAppliedStructType(field.type);
          }
        }

        for (auto& function : module.functions) {
          function.returnType = rewriteAppliedStructType(function.returnType);
          for (auto& parameter : function.parameters) {
            parameter.type = rewriteAppliedStructType(parameter.type);
          }
          for (const auto& statement : function.body) {
            statement->accept(*this);
          }
        }
      }

      void visit(const ast::ReturnStatement& node) override { node.expression->accept(*this); }
      void visit(const ast::LetStatement& node) override {
        const_cast<ast::LetStatement&>(node).type = rewriteAppliedStructType(node.type);
        node.initializer->accept(*this);
      }
      void visit(const ast::IfStatement& node) override {
        node.condition->accept(*this);
        visitStatements(node.thenBranch);
        visitStatements(node.elseBranch);
      }
      void visit(const ast::WhileStatement& node) override {
        node.condition->accept(*this);
        visitStatements(node.body);
      }
      void visit(const ast::AssignmentStatement& node) override {
        node.lhs->accept(*this);
        node.rhs->accept(*this);
      }
      void visit(const ast::ExpressionStatement& node) override { node.expression->accept(*this); }

      void visit(const ast::UnaryExpression& node) override { node.operand->accept(*this); }
      void visit(const ast::CastExpression& node) override {
        const_cast<ast::CastExpression&>(node).targetType =
            rewriteAppliedStructType(node.targetType);
        node.expression->accept(*this);
      }
      void visit(const ast::BinaryExpression& node) override {
        node.left->accept(*this);
        node.right->accept(*this);
      }
      void visit(const ast::CallExpression& node) override {
        for (const auto& argument : node.arguments) {
          argument->accept(*this);
        }
      }
      void visit(const ast::ArrayLiteral& node) override {
        for (const auto& element : node.elements) {
          element->accept(*this);
        }
      }
      void visit(const ast::IndexExpression& node) override {
        node.base->accept(*this);
        node.index->accept(*this);
      }
      void visit(const ast::StructLiteral& node) override {
        ast::StructLiteral& literal = const_cast<ast::StructLiteral&>(node);
        if (!literal.typeArgs.empty()) {
          literal.structName = mangleSpecialization(literal.structName, literal.typeArgs);
          literal.typeArgs.clear();
          for (const auto& field : literal.fields) {
            field.value->accept(*this);
          }
          return;
        }

        for (const auto& request : requests_) {
          if (literal.location.line == request.useSiteLocation.line &&
              literal.location.column == request.useSiteLocation.column &&
              literal.structName == request.templateName) {
            literal.structName = mangleSpecialization(request.templateName, request.typeArgs);
            break;
          }
        }

        for (const auto& field : literal.fields) {
          field.value->accept(*this);
        }
      }
      void visit(const ast::FieldAccessExpression& node) override { node.base->accept(*this); }

      void visit(const ast::IntegerLiteral&) override {}
      void visit(const ast::FloatLiteral&) override {}
      void visit(const ast::StringLiteral&) override {}
      void visit(const ast::BoolLiteral&) override {}
      void visit(const ast::IdentifierExpression&) override {}

    private:
      void visitStatements(const std::vector<std::unique_ptr<ast::Statement>>& statements) {
        for (const auto& statement : statements) {
          statement->accept(*this);
        }
      }

      const std::vector<StructSpecializationRequest>& requests_;
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
      specialized.returnType = substituteType(templated.returnType, substitution);
      specialized.location = templated.location;
      specialized.typeParams = {};

      specialized.parameters.reserve(templated.parameters.size());
      for (const auto& parameter : templated.parameters) {
        specialized.parameters.push_back(ast::Parameter{
            parameter.name, substituteType(parameter.type, substitution), parameter.location});
      }

      CloneVisitor cloner(templated.name, typeArgs, substitution);
      specialized.body = cloner.cloneStatements(templated.body);
      return specialized;
    }

  } // namespace

  Type substitute(const Type& type, const Substitution& substitution) {
    return substituteType(type, substitution);
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
      throw CompileError("internal: cannot mangle unsubstituted type parameter");
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

  std::size_t expandSpecializations(ast::Module& module,
                                    const std::vector<SpecializationRequest>& requests) {
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
                return left.callSiteLocation.line < right.callSiteLocation.line;
              });

    std::unordered_set<std::string> seen;
    std::size_t added = 0;

    module.functions.reserve(module.functions.size() + sorted.size());
    for (const SpecializationRequest& request : sorted) {
      const std::string mangledName = mangleSpecialization(request.templateName, request.typeArgs);
      if (!seen.insert(mangledName).second) {
        continue;
      }
      if (hasFunctionSpecialization(module, mangledName)) {
        continue;
      }

      const ast::Function* templated = findTemplateFunction(module, request.templateName);
      if (templated == nullptr) {
        throw CompileError(
            formatDiagnostic(request.callSiteLocation, DiagnosticStage::TypeCheck,
                             "unknown generic function '" + request.templateName + "'"));
      }

      module.functions.push_back(cloneSpecialization(*templated, request.typeArgs));
      ++added;
    }

    rewriteGenericCallSites(module, requests);
    return added;
  }

  std::size_t
  expandStructSpecializations(ast::Module& module,
                              const std::vector<StructSpecializationRequest>& requests) {
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
          return left.useSiteLocation.line < right.useSiteLocation.line;
        });

    std::unordered_set<std::string> seen;
    std::size_t added = 0;

    module.structs.reserve(module.structs.size() + sorted.size());
    for (const StructSpecializationRequest& request : sorted) {
      const std::string mangledName = mangleSpecialization(request.templateName, request.typeArgs);
      if (!seen.insert(mangledName).second) {
        continue;
      }
      if (hasStructSpecialization(module, mangledName)) {
        continue;
      }

      const ast::StructDecl* templated = findTemplateStruct(module, request.templateName);
      if (templated == nullptr) {
        throw CompileError(
            formatDiagnostic(request.useSiteLocation, DiagnosticStage::TypeCheck,
                             "unknown generic struct '" + request.templateName + "'"));
      }

      module.structs.push_back(cloneStructSpecialization(*templated, request.typeArgs));
      ++added;
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
