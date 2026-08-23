#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Token.hpp"
#include "noria/AstVisitor.hpp"
#include "noria/Types.hpp"

namespace noria::ast {

  struct Expression {
    explicit Expression(SourceLocation location) : location(location) {}
    virtual ~Expression() = default;

    virtual void accept(AstVisitor& visitor) const = 0;

    SourceLocation location;
  };

  struct IntegerLiteral final : Expression {
    IntegerLiteral(std::int64_t value, SourceLocation location)
        : Expression(location), value(value) {}

    void accept(AstVisitor& visitor) const override { visitor.visit(*this); }

    std::int64_t value;
  };

  struct FloatLiteral final : Expression {
    FloatLiteral(double value, SourceLocation location) : Expression(location), value(value) {}

    void accept(AstVisitor& visitor) const override { visitor.visit(*this); }

    double value;
  };

  struct StringLiteral final : Expression {
    StringLiteral(std::string value, SourceLocation location)
        : Expression(location), value(std::move(value)) {}

    void accept(AstVisitor& visitor) const override { visitor.visit(*this); }

    std::string value;
  };

  struct BoolLiteral final : Expression {
    BoolLiteral(bool value, SourceLocation location) : Expression(location), value(value) {}

    void accept(AstVisitor& visitor) const override { visitor.visit(*this); }

    bool value;
  };

  enum class BinaryOperator {
    Add,
    Subtract,
    Multiply,
    Divide,
    Modulo,
    And,
    Or,
    BitAnd,
    BitOr,
    BitXor,
    Shl,
    Shr,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Equal,
    NotEqual,
  };

  enum class UnaryOperator {
    Negate,
    Not,
    BitNot,
  };

  struct UnaryExpression final : Expression {
    UnaryExpression(UnaryOperator op, std::unique_ptr<Expression> operand, SourceLocation location)
        : Expression(location), op(op), operand(std::move(operand)) {}

    void accept(AstVisitor& visitor) const override { visitor.visit(*this); }

    UnaryOperator op;
    std::unique_ptr<Expression> operand;
  };

  struct CastExpression final : Expression {
    CastExpression(std::unique_ptr<Expression> expression, Type targetType, SourceLocation location)
        : Expression(location), expression(std::move(expression)),
          targetType(std::move(targetType)) {}

    void accept(AstVisitor& visitor) const override { visitor.visit(*this); }

    std::unique_ptr<Expression> expression;
    Type targetType;
  };

  struct BinaryExpression final : Expression {
    BinaryExpression(BinaryOperator op, std::unique_ptr<Expression> left,
                     std::unique_ptr<Expression> right, SourceLocation location)
        : Expression(location), op(op), left(std::move(left)), right(std::move(right)) {}

    void accept(AstVisitor& visitor) const override { visitor.visit(*this); }

    BinaryOperator op;
    std::unique_ptr<Expression> left;
    std::unique_ptr<Expression> right;
  };

  struct IdentifierExpression final : Expression {
    IdentifierExpression(std::string name, SourceLocation location)
        : Expression(location), name(std::move(name)) {}

    void accept(AstVisitor& visitor) const override { visitor.visit(*this); }

    std::string name;
  };

  struct CallExpression final : Expression {
    CallExpression(std::string callee, std::vector<std::unique_ptr<Expression>> arguments,
                   SourceLocation location)
        : Expression(location), callee(std::move(callee)), arguments(std::move(arguments)) {}

    void accept(AstVisitor& visitor) const override { visitor.visit(*this); }

    std::string callee;
    std::vector<std::unique_ptr<Expression>> arguments;
  };

  struct ArrayLiteral final : Expression {
    ArrayLiteral(std::vector<std::unique_ptr<Expression>> elements, SourceLocation location)
        : Expression(location), elements(std::move(elements)) {}

    void accept(AstVisitor& visitor) const override { visitor.visit(*this); }

    std::vector<std::unique_ptr<Expression>> elements;
  };

  struct IndexExpression final : Expression {
    IndexExpression(std::unique_ptr<Expression> base, std::unique_ptr<Expression> index,
                    SourceLocation location)
        : Expression(location), base(std::move(base)), index(std::move(index)) {}

    void accept(AstVisitor& visitor) const override { visitor.visit(*this); }

    std::unique_ptr<Expression> base;
    std::unique_ptr<Expression> index;
  };

  struct StructLiteralField {
    std::string name;
    std::unique_ptr<Expression> value;
    SourceLocation location;
  };

  struct StructLiteral final : Expression {
    StructLiteral(std::string structName, std::vector<Type> typeArgs,
                  std::vector<StructLiteralField> fields, SourceLocation location)
        : Expression(location), structName(std::move(structName)), typeArgs(std::move(typeArgs)),
          fields(std::move(fields)) {}

    void accept(AstVisitor& visitor) const override { visitor.visit(*this); }

    std::string structName;
    std::vector<Type> typeArgs;
    std::vector<StructLiteralField> fields;
  };

  struct FieldAccessExpression final : Expression {
    FieldAccessExpression(std::unique_ptr<Expression> base, std::string fieldName,
                          SourceLocation location)
        : Expression(location), base(std::move(base)), fieldName(std::move(fieldName)) {}

    void accept(AstVisitor& visitor) const override { visitor.visit(*this); }

    std::unique_ptr<Expression> base;
    std::string fieldName;
  };

  struct Statement {
    explicit Statement(SourceLocation location) : location(location) {}
    virtual ~Statement() = default;

    virtual void accept(AstVisitor& visitor) const = 0;

    SourceLocation location;
  };

  struct ReturnStatement final : Statement {
    ReturnStatement(std::unique_ptr<Expression> expression, SourceLocation location)
        : Statement(location), expression(std::move(expression)) {}

    void accept(AstVisitor& visitor) const override { visitor.visit(*this); }

    std::unique_ptr<Expression> expression;
  };

  struct LetStatement final : Statement {
    LetStatement(std::string name, Type type, std::unique_ptr<Expression> initializer,
                 SourceLocation location)
        : Statement(location), name(std::move(name)), type(std::move(type)),
          initializer(std::move(initializer)) {}

    void accept(AstVisitor& visitor) const override { visitor.visit(*this); }

    std::string name;
    Type type;
    std::unique_ptr<Expression> initializer;
  };

  struct IfStatement final : Statement {
    IfStatement(std::unique_ptr<Expression> condition,
                std::vector<std::unique_ptr<Statement>> thenBranch,
                std::vector<std::unique_ptr<Statement>> elseBranch, SourceLocation location)
        : Statement(location), condition(std::move(condition)), thenBranch(std::move(thenBranch)),
          elseBranch(std::move(elseBranch)) {}

    void accept(AstVisitor& visitor) const override { visitor.visit(*this); }

    std::unique_ptr<Expression> condition;
    std::vector<std::unique_ptr<Statement>> thenBranch;
    std::vector<std::unique_ptr<Statement>> elseBranch;
  };

  struct WhileStatement final : Statement {
    WhileStatement(std::unique_ptr<Expression> condition,
                   std::vector<std::unique_ptr<Statement>> body, SourceLocation location)
        : Statement(location), condition(std::move(condition)), body(std::move(body)) {}

    void accept(AstVisitor& visitor) const override { visitor.visit(*this); }

    std::unique_ptr<Expression> condition;
    std::vector<std::unique_ptr<Statement>> body;
  };

  struct AssignmentStatement final : Statement {
    AssignmentStatement(std::unique_ptr<Expression> lhs, std::unique_ptr<Expression> rhs,
                        SourceLocation location)
        : Statement(location), lhs(std::move(lhs)), rhs(std::move(rhs)) {}

    void accept(AstVisitor& visitor) const override { visitor.visit(*this); }

    std::unique_ptr<Expression> lhs;
    std::unique_ptr<Expression> rhs;
  };

  struct ExpressionStatement final : Statement {
    ExpressionStatement(std::unique_ptr<Expression> expression, SourceLocation location)
        : Statement(location), expression(std::move(expression)) {}

    void accept(AstVisitor& visitor) const override { visitor.visit(*this); }

    std::unique_ptr<Expression> expression;
  };

  struct Parameter {
    std::string name;
    Type type;
    SourceLocation location;
  };

  struct TypeParameter {
    std::string name;
    SourceLocation location;
  };

  struct StructField {
    std::string name;
    Type type;
    SourceLocation location;
  };

  struct StructDecl {
    std::string name;
    std::vector<TypeParameter> typeParams;
    std::vector<StructField> fields;
    SourceLocation location;
  };

  struct Function {
    std::string name;
    Type returnType;
    SourceLocation location;
    std::vector<TypeParameter> typeParams;
    std::vector<Parameter> parameters;
    std::vector<std::unique_ptr<Statement>> body;
  };

  struct ImportedName {
    std::string name;
    SourceLocation location;
  };

  struct ImportDecl {
    std::vector<std::string> path;
    std::vector<ImportedName> names;
    SourceLocation location;
  };

  struct Module {
    std::vector<ImportDecl> imports;
    std::vector<StructDecl> structs;
    std::vector<Function> functions;
  };

} // namespace noria::ast
