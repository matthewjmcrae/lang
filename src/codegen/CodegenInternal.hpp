#pragma once

#include "noria/Codegen.hpp"

#include "../internal/AstVisitorAdapters.hpp"
#include "noria/AstVisitor.hpp"

namespace noria {
  class LLVMGenerator::StatementVisitor final : public internal::StatementOnlyVisitor {
  public:
    using internal::StatementOnlyVisitor::visit;
    StatementVisitor(const LLVMGenerator&, IREmitter&, FunctionCodegenContext&, Type,
                     std::vector<Scope>&);
    bool returned() const { return returned_; }
    void visit(const ast::ReturnStatement&) override; void visit(const ast::LetStatement&) override;
    void visit(const ast::IfStatement&) override; void visit(const ast::WhileStatement&) override;
    void visit(const ast::AssignmentStatement&) override; void visit(const ast::ExpressionStatement&) override;
  private:
    const LLVMGenerator& generator_; IREmitter& emitter_; FunctionCodegenContext& context_;
    Type expectedReturnType_; std::vector<Scope>& scopes_; bool returned_ = false;
  };
  class LLVMGenerator::ExpressionVisitor final : public internal::ExpressionOnlyVisitor {
  public:
    using internal::ExpressionOnlyVisitor::visit;
    ExpressionVisitor(const LLVMGenerator&, IREmitter&, FunctionCodegenContext&, const std::vector<Scope>&);
    Value result() const { return result_; }
    void visit(const ast::IntegerLiteral&) override; void visit(const ast::FloatLiteral&) override;
    void visit(const ast::StringLiteral&) override; void visit(const ast::BoolLiteral&) override;
    void visit(const ast::UnaryExpression&) override; void visit(const ast::CastExpression&) override;
    void visit(const ast::BinaryExpression&) override; void visit(const ast::IdentifierExpression&) override;
    void visit(const ast::CallExpression&) override; void visit(const ast::ArrayLiteral&) override;
    void visit(const ast::IndexExpression&) override; void visit(const ast::StructLiteral&) override;
    void visit(const ast::FieldAccessExpression&) override;
  private:
    const LLVMGenerator& generator_; IREmitter& emitter_; FunctionCodegenContext& context_;
    const std::vector<Scope>& scopes_; Value result_{};
  };
  class LLVMGenerator::PlaceVisitor final : public ast::AstVisitor {
  public:
    PlaceVisitor(const LLVMGenerator&, IREmitter&, FunctionCodegenContext&, const std::vector<Scope>&);
    LocalBinding result() const { return result_; }
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
    const LLVMGenerator& generator_; IREmitter& emitter_; FunctionCodegenContext& context_;
    const std::vector<Scope>& scopes_; LocalBinding result_{};
  };
} // namespace noria
