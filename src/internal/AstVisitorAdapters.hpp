#pragma once

#include "noria/Ast.hpp"
#include "noria/Diagnostic.hpp"

#include <string>
#include <string_view>

namespace noria::internal {

  class StatementOnlyVisitor : public ast::AstVisitor {
  public:
    explicit StatementOnlyVisitor(std::string_view stage) : stage_(stage) {}

    void visit(const ast::IntegerLiteral&) final { unsupportedExpression(); }
    void visit(const ast::FloatLiteral&) final { unsupportedExpression(); }
    void visit(const ast::StringLiteral&) final { unsupportedExpression(); }
    void visit(const ast::BoolLiteral&) final { unsupportedExpression(); }
    void visit(const ast::UnaryExpression&) final { unsupportedExpression(); }
    void visit(const ast::CastExpression&) final { unsupportedExpression(); }
    void visit(const ast::BinaryExpression&) final { unsupportedExpression(); }
    void visit(const ast::IdentifierExpression&) final { unsupportedExpression(); }
    void visit(const ast::CallExpression&) final { unsupportedExpression(); }
    void visit(const ast::ArrayLiteral&) final { unsupportedExpression(); }
    void visit(const ast::IndexExpression&) final { unsupportedExpression(); }
    void visit(const ast::StructLiteral&) final { unsupportedExpression(); }
    void visit(const ast::FieldAccessExpression&) final { unsupportedExpression(); }

  private:
    [[noreturn]] void unsupportedExpression() const {
      throw CompileError(stage_ + ": internal error: expression visited by statement visitor");
    }

    std::string stage_;
  };

  class ExpressionOnlyVisitor : public ast::AstVisitor {
  public:
    explicit ExpressionOnlyVisitor(std::string_view stage) : stage_(stage) {}

    void visit(const ast::ReturnStatement&) final { unsupportedStatement(); }
    void visit(const ast::LetStatement&) final { unsupportedStatement(); }
    void visit(const ast::IfStatement&) final { unsupportedStatement(); }
    void visit(const ast::WhileStatement&) final { unsupportedStatement(); }
    void visit(const ast::AssignmentStatement&) final { unsupportedStatement(); }
    void visit(const ast::ExpressionStatement&) final { unsupportedStatement(); }

  private:
    [[noreturn]] void unsupportedStatement() const {
      throw CompileError(stage_ + ": internal error: statement visited by expression visitor");
    }

    std::string stage_;
  };

  class RecursiveAstMutator : public ast::AstMutator {
  public:
    void visit(ast::IntegerLiteral&) override {}
    void visit(ast::FloatLiteral&) override {}
    void visit(ast::StringLiteral&) override {}
    void visit(ast::BoolLiteral&) override {}
    void visit(ast::IdentifierExpression&) override {}

    void visit(ast::UnaryExpression& node) override { node.operand->accept(*this); }
    void visit(ast::CastExpression& node) override { node.expression->accept(*this); }
    void visit(ast::BinaryExpression& node) override {
      node.left->accept(*this);
      node.right->accept(*this);
    }
    void visit(ast::CallExpression& node) override { visitExpressions(node.arguments); }
    void visit(ast::ArrayLiteral& node) override { visitExpressions(node.elements); }
    void visit(ast::IndexExpression& node) override {
      node.base->accept(*this);
      node.index->accept(*this);
    }
    void visit(ast::StructLiteral& node) override {
      for (auto& field : node.fields) {
        field.value->accept(*this);
      }
    }
    void visit(ast::FieldAccessExpression& node) override { node.base->accept(*this); }

    void visit(ast::ReturnStatement& node) override {
      if (node.expression) {
        node.expression->accept(*this);
      }
    }
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

  protected:
    void visitStatements(const std::vector<std::unique_ptr<ast::Statement>>& statements) {
      for (const auto& statement : statements) {
        statement->accept(*this);
      }
    }

    void visitExpressions(const std::vector<std::unique_ptr<ast::Expression>>& expressions) {
      for (const auto& expression : expressions) {
        expression->accept(*this);
      }
    }
  };

} // namespace noria::internal
