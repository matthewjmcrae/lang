#pragma once

#include "noria/TypeChecker.hpp"

#include "../internal/AstVisitorAdapters.hpp"
#include "noria/AstVisitor.hpp"

namespace noria {
  class TypeChecker::StatementVisitor final : public internal::StatementOnlyVisitor {
  public:
    using internal::StatementOnlyVisitor::visit;
    StatementVisitor(TypeChecker&, Type);
    bool returned() const { return returned_; }
    void visit(const ast::ReturnStatement&) override; void visit(const ast::LetStatement&) override;
    void visit(const ast::IfStatement&) override; void visit(const ast::WhileStatement&) override;
    void visit(const ast::AssignmentStatement&) override; void visit(const ast::ExpressionStatement&) override;
  private:
    TypeChecker& checker_; Type expectedReturnType_; bool returned_ = false;
  };
  class TypeChecker::ExpressionVisitor final : public internal::ExpressionOnlyVisitor {
  public:
    using internal::ExpressionOnlyVisitor::visit;
    ExpressionVisitor(TypeChecker&, std::optional<Type>);
    Type result() const { return result_; }
    void visit(const ast::IntegerLiteral&) override; void visit(const ast::FloatLiteral&) override;
    void visit(const ast::StringLiteral&) override; void visit(const ast::BoolLiteral&) override;
    void visit(const ast::UnaryExpression&) override; void visit(const ast::CastExpression&) override;
    void visit(const ast::BinaryExpression&) override; void visit(const ast::IdentifierExpression&) override;
    void visit(const ast::CallExpression&) override; void visit(const ast::ArrayLiteral&) override;
    void visit(const ast::IndexExpression&) override; void visit(const ast::StructLiteral&) override;
    void visit(const ast::FieldAccessExpression&) override;
  private:
    TypeChecker& checker_; std::optional<Type> expectedType_; Type result_;
  };
  class TypeChecker::PlaceVisitor final : public ast::AstVisitor {
  public:
    explicit PlaceVisitor(TypeChecker&);
    const std::string& name() const { return name_; }
    Type type() const { return type_; }
    void visit(const ast::IdentifierExpression&) override;
    void visit(const ast::IntegerLiteral&) override; void visit(const ast::FloatLiteral&) override;
    void visit(const ast::StringLiteral&) override; void visit(const ast::BoolLiteral&) override;
    void visit(const ast::UnaryExpression&) override; void visit(const ast::CastExpression&) override;
    void visit(const ast::BinaryExpression&) override; void visit(const ast::CallExpression&) override;
    void visit(const ast::ArrayLiteral&) override; void visit(const ast::IndexExpression&) override;
    void visit(const ast::StructLiteral&) override; void visit(const ast::FieldAccessExpression&) override;
    void visit(const ast::ReturnStatement&) override; void visit(const ast::LetStatement&) override;
    void visit(const ast::IfStatement&) override; void visit(const ast::WhileStatement&) override;
    void visit(const ast::AssignmentStatement&) override; void visit(const ast::ExpressionStatement&) override;
  private:
    TypeChecker& checker_; std::string name_; Type type_;
  };
} // namespace noria
