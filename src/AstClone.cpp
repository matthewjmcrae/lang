#include "noria/AstClone.hpp"

#include "noria/AstVisitor.hpp"

#include <memory>
#include <utility>

namespace noria::ast {

  namespace {

    class CloneVisitor final : public AstVisitor {
    public:
      std::unique_ptr<Expression> takeExpression() { return std::move(expression_); }
      std::unique_ptr<Statement> takeStatement() { return std::move(statement_); }

      void visit(const IntegerLiteral& node) override {
        expression_ = std::make_unique<IntegerLiteral>(node.value, node.location);
      }

      void visit(const FloatLiteral& node) override {
        expression_ = std::make_unique<FloatLiteral>(node.value, node.location);
      }

      void visit(const StringLiteral& node) override {
        expression_ = std::make_unique<StringLiteral>(node.value, node.location);
      }

      void visit(const BoolLiteral& node) override {
        expression_ = std::make_unique<BoolLiteral>(node.value, node.location);
      }

      void visit(const UnaryExpression& node) override {
        expression_ =
            std::make_unique<UnaryExpression>(node.op, cloneChild(*node.operand), node.location);
      }

      void visit(const CastExpression& node) override {
        expression_ =
            std::make_unique<CastExpression>(cloneChild(*node.expression), node.targetType,
                                             node.location);
      }

      void visit(const BinaryExpression& node) override {
        auto left = cloneChild(*node.left);
        expression_ = std::make_unique<BinaryExpression>(node.op, std::move(left),
                                                         cloneChild(*node.right), node.location);
      }

      void visit(const IdentifierExpression& node) override {
        expression_ = std::make_unique<IdentifierExpression>(node.name, node.location);
      }

      void visit(const CallExpression& node) override {
        expression_ =
            std::make_unique<CallExpression>(node.callee, cloneExpressionList(node.arguments),
                                             node.location);
      }

      void visit(const ArrayLiteral& node) override {
        expression_ =
            std::make_unique<ArrayLiteral>(cloneExpressionList(node.elements), node.location);
      }

      void visit(const IndexExpression& node) override {
        auto base = cloneChild(*node.base);
        expression_ =
            std::make_unique<IndexExpression>(std::move(base), cloneChild(*node.index),
                                              node.location);
      }

      void visit(const StructLiteral& node) override {
        std::vector<StructLiteralField> fields;
        fields.reserve(node.fields.size());
        for (const auto& field : node.fields) {
          fields.push_back(cloneStructLiteralFieldChild(field));
        }
        expression_ =
            std::make_unique<StructLiteral>(node.structName, node.typeArgs, std::move(fields),
                                            node.location);
      }

      void visit(const FieldAccessExpression& node) override {
        expression_ = std::make_unique<FieldAccessExpression>(
            cloneChild(*node.base), node.fieldName, node.location);
      }

      void visit(const ReturnStatement& node) override {
        statement_ = std::make_unique<ReturnStatement>(cloneChild(*node.expression), node.location);
      }

      void visit(const LetStatement& node) override {
        std::unique_ptr<Expression> initializer;
        if (node.initializer) {
          initializer = cloneChild(*node.initializer);
        }
        statement_ = std::make_unique<LetStatement>(
            node.name, node.declaredType, std::move(initializer), node.location);
      }

      void visit(const IfStatement& node) override {
        statement_ = std::make_unique<IfStatement>(cloneChild(*node.condition),
                                                   cloneStatementList(node.thenBranch),
                                                   cloneStatementList(node.elseBranch),
                                                   node.location);
      }

      void visit(const WhileStatement& node) override {
        statement_ = std::make_unique<WhileStatement>(cloneChild(*node.condition),
                                                      cloneStatementList(node.body),
                                                      node.location);
      }

      void visit(const AssignmentStatement& node) override {
        auto lhs = cloneChild(*node.lhs);
        statement_ =
            std::make_unique<AssignmentStatement>(std::move(lhs), cloneChild(*node.rhs),
                                                  node.location);
      }

      void visit(const ExpressionStatement& node) override {
        statement_ =
            std::make_unique<ExpressionStatement>(cloneChild(*node.expression), node.location);
      }

    private:
      std::unique_ptr<Expression> cloneChild(const Expression& expression) {
        expression.accept(*this);
        return takeExpression();
      }

      std::unique_ptr<Statement> cloneChild(const Statement& statement) {
        statement.accept(*this);
        return takeStatement();
      }

      std::vector<std::unique_ptr<Expression>>
      cloneExpressionList(const std::vector<std::unique_ptr<Expression>>& expressions) {
        std::vector<std::unique_ptr<Expression>> cloned;
        cloned.reserve(expressions.size());
        for (const auto& expression : expressions) {
          cloned.push_back(cloneChild(*expression));
        }
        return cloned;
      }

      std::vector<std::unique_ptr<Statement>>
      cloneStatementList(const std::vector<std::unique_ptr<Statement>>& statements) {
        std::vector<std::unique_ptr<Statement>> cloned;
        cloned.reserve(statements.size());
        for (const auto& statement : statements) {
          cloned.push_back(cloneChild(*statement));
        }
        return cloned;
      }

      StructLiteralField cloneStructLiteralFieldChild(const StructLiteralField& field) {
        return StructLiteralField{field.name, cloneChild(*field.value), field.location};
      }

      std::unique_ptr<Expression> expression_;
      std::unique_ptr<Statement> statement_;
    };

  } // namespace

  std::unique_ptr<Expression> cloneExpression(const Expression& expression) {
    CloneVisitor cloner;
    expression.accept(cloner);
    return cloner.takeExpression();
  }

  std::unique_ptr<Statement> cloneStatement(const Statement& statement) {
    CloneVisitor cloner;
    statement.accept(cloner);
    return cloner.takeStatement();
  }

  std::vector<std::unique_ptr<Expression>>
  cloneExpressions(const std::vector<std::unique_ptr<Expression>>& expressions) {
    std::vector<std::unique_ptr<Expression>> cloned;
    cloned.reserve(expressions.size());
    for (const auto& expression : expressions) {
      cloned.push_back(cloneExpression(*expression));
    }
    return cloned;
  }

  std::vector<std::unique_ptr<Statement>>
  cloneStatements(const std::vector<std::unique_ptr<Statement>>& statements) {
    std::vector<std::unique_ptr<Statement>> cloned;
    cloned.reserve(statements.size());
    for (const auto& statement : statements) {
      cloned.push_back(cloneStatement(*statement));
    }
    return cloned;
  }

  StructLiteralField cloneStructLiteralField(const StructLiteralField& field) {
    return StructLiteralField{field.name, cloneExpression(*field.value), field.location};
  }

  Parameter cloneParameter(const Parameter& parameter) {
    return Parameter{parameter.name, parameter.type, parameter.location};
  }

  TypeParameter cloneTypeParameter(const TypeParameter& typeParameter) {
    return TypeParameter{typeParameter.name, typeParameter.location};
  }

  StructField cloneStructField(const StructField& field) {
    return StructField{field.name, field.type, field.location, field.visibility};
  }

  StructDecl cloneStructDecl(const StructDecl& structDecl) {
    StructDecl cloned;
    cloned.name = structDecl.name;
    cloned.location = structDecl.location;
    cloned.typeParams.reserve(structDecl.typeParams.size());
    for (const auto& typeParam : structDecl.typeParams) {
      cloned.typeParams.push_back(cloneTypeParameter(typeParam));
    }
    cloned.fields.reserve(structDecl.fields.size());
    for (const auto& field : structDecl.fields) {
      cloned.fields.push_back(cloneStructField(field));
    }
    return cloned;
  }

  Function cloneFunction(const Function& function) {
    Function cloned;
    cloned.name = function.name;
    cloned.returnType = function.returnType;
    cloned.location = function.location;
    cloned.implTag = function.implTag;
    cloned.typeParams.reserve(function.typeParams.size());
    for (const auto& typeParam : function.typeParams) {
      cloned.typeParams.push_back(cloneTypeParameter(typeParam));
    }
    cloned.parameters.reserve(function.parameters.size());
    for (const auto& parameter : function.parameters) {
      cloned.parameters.push_back(cloneParameter(parameter));
    }
    cloned.body = cloneStatements(function.body);
    return cloned;
  }

  ImportedName cloneImportedName(const ImportedName& importedName) {
    return ImportedName{importedName.name, importedName.location};
  }

  ImportDecl cloneImportDecl(const ImportDecl& importDecl) {
    ImportDecl cloned;
    cloned.path = importDecl.path;
    cloned.location = importDecl.location;
    cloned.names.reserve(importDecl.names.size());
    for (const auto& importedName : importDecl.names) {
      cloned.names.push_back(cloneImportedName(importedName));
    }
    return cloned;
  }

  Module cloneModule(const Module& module) {
    Module cloned;
    cloned.imports.reserve(module.imports.size());
    for (const auto& importDecl : module.imports) {
      cloned.imports.push_back(cloneImportDecl(importDecl));
    }
    cloned.structs.reserve(module.structs.size());
    for (const auto& structDecl : module.structs) {
      cloned.structs.push_back(cloneStructDecl(structDecl));
    }
    cloned.functions.reserve(module.functions.size());
    for (const auto& function : module.functions) {
      cloned.functions.push_back(cloneFunction(function));
    }
    return cloned;
  }

} // namespace noria::ast
