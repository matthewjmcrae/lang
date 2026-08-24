#pragma once

namespace noria::ast {

  struct IntegerLiteral;
  struct FloatLiteral;
  struct StringLiteral;
  struct BoolLiteral;
  struct UnaryExpression;
  struct CastExpression;
  struct BinaryExpression;
  struct IdentifierExpression;
  struct CallExpression;
  struct ArrayLiteral;
  struct IndexExpression;
  struct StructLiteral;
  struct FieldAccessExpression;

  struct ReturnStatement;
  struct LetStatement;
  struct IfStatement;
  struct WhileStatement;
  struct AssignmentStatement;
  struct ExpressionStatement;
  struct ASTNode;

  class AstVisitor {
  public:
    virtual ~AstVisitor() = default;

    void visit(const ASTNode& node);

    virtual void visit(const IntegerLiteral& node) = 0;
    virtual void visit(const FloatLiteral& node) = 0;
    virtual void visit(const StringLiteral& node) = 0;
    virtual void visit(const BoolLiteral& node) = 0;
    virtual void visit(const UnaryExpression& node) = 0;
    virtual void visit(const CastExpression& node) = 0;
    virtual void visit(const BinaryExpression& node) = 0;
    virtual void visit(const IdentifierExpression& node) = 0;
    virtual void visit(const CallExpression& node) = 0;
    virtual void visit(const ArrayLiteral& node) = 0;
    virtual void visit(const IndexExpression& node) = 0;
    virtual void visit(const StructLiteral& node) = 0;
    virtual void visit(const FieldAccessExpression& node) = 0;

    virtual void visit(const ReturnStatement& node) = 0;
    virtual void visit(const LetStatement& node) = 0;
    virtual void visit(const IfStatement& node) = 0;
    virtual void visit(const WhileStatement& node) = 0;
    virtual void visit(const AssignmentStatement& node) = 0;
    virtual void visit(const ExpressionStatement& node) = 0;
  };

  class AstMutator {
  public:
    virtual ~AstMutator() = default;

    void visit(ASTNode& node);

    virtual void visit(IntegerLiteral& node) = 0;
    virtual void visit(FloatLiteral& node) = 0;
    virtual void visit(StringLiteral& node) = 0;
    virtual void visit(BoolLiteral& node) = 0;
    virtual void visit(UnaryExpression& node) = 0;
    virtual void visit(CastExpression& node) = 0;
    virtual void visit(BinaryExpression& node) = 0;
    virtual void visit(IdentifierExpression& node) = 0;
    virtual void visit(CallExpression& node) = 0;
    virtual void visit(ArrayLiteral& node) = 0;
    virtual void visit(IndexExpression& node) = 0;
    virtual void visit(StructLiteral& node) = 0;
    virtual void visit(FieldAccessExpression& node) = 0;

    virtual void visit(ReturnStatement& node) = 0;
    virtual void visit(LetStatement& node) = 0;
    virtual void visit(IfStatement& node) = 0;
    virtual void visit(WhileStatement& node) = 0;
    virtual void visit(AssignmentStatement& node) = 0;
    virtual void visit(ExpressionStatement& node) = 0;
  };

} // namespace noria::ast
