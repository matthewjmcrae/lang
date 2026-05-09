#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Token.hpp"

namespace noria::ast {

  struct Expression {
    explicit Expression(SourceLocation location) : location(location) {}
    virtual ~Expression() = default;

    SourceLocation location;
  };

  struct IntegerLiteral final : Expression {
    IntegerLiteral(std::int64_t value, SourceLocation location)
        : Expression(location), value(value) {}

    std::int64_t value;
  };

  struct BoolLiteral final : Expression {
    BoolLiteral(bool value, SourceLocation location) : Expression(location), value(value) {}

    bool value;
  };

  enum class BinaryOperator {
    Add,
    Subtract,
    Multiply,
    Divide,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Equal,
    NotEqual,
  };

  struct BinaryExpression final : Expression {
    BinaryExpression(BinaryOperator op, std::unique_ptr<Expression> left,
                     std::unique_ptr<Expression> right, SourceLocation location)
        : Expression(location), op(op), left(std::move(left)), right(std::move(right)) {}

    BinaryOperator op;
    std::unique_ptr<Expression> left;
    std::unique_ptr<Expression> right;
  };

  struct IdentifierExpression final : Expression {
    IdentifierExpression(std::string name, SourceLocation location)
        : Expression(location), name(std::move(name)) {}

    std::string name;
  };

  struct CallExpression final : Expression {
    CallExpression(std::string callee, std::vector<std::unique_ptr<Expression>> arguments,
                   SourceLocation location)
        : Expression(location), callee(std::move(callee)), arguments(std::move(arguments)) {}

    std::string callee;
    std::vector<std::unique_ptr<Expression>> arguments;
  };

  struct Statement {
    explicit Statement(SourceLocation location) : location(location) {}
    virtual ~Statement() = default;

    SourceLocation location;
  };

  struct ReturnStatement final : Statement {
    ReturnStatement(std::unique_ptr<Expression> expression, SourceLocation location)
        : Statement(location), expression(std::move(expression)) {}

    std::unique_ptr<Expression> expression;
  };

  struct LetStatement final : Statement {
    LetStatement(std::string name, std::string typeName, std::unique_ptr<Expression> initializer,
                 SourceLocation location)
        : Statement(location), name(std::move(name)), typeName(std::move(typeName)),
          initializer(std::move(initializer)) {}

    std::string name;
    std::string typeName;
    std::unique_ptr<Expression> initializer;
  };

  struct IfStatement final : Statement {
    IfStatement(std::unique_ptr<Expression> condition,
                std::vector<std::unique_ptr<Statement>> thenBranch,
                std::vector<std::unique_ptr<Statement>> elseBranch, SourceLocation location)
        : Statement(location), condition(std::move(condition)), thenBranch(std::move(thenBranch)),
          elseBranch(std::move(elseBranch)) {}

    std::unique_ptr<Expression> condition;
    std::vector<std::unique_ptr<Statement>> thenBranch;
    std::vector<std::unique_ptr<Statement>> elseBranch;
  };

  struct WhileStatement final : Statement {
    WhileStatement(std::unique_ptr<Expression> condition,
                   std::vector<std::unique_ptr<Statement>> body, SourceLocation location)
        : Statement(location), condition(std::move(condition)), body(std::move(body)) {}

    std::unique_ptr<Expression> condition;
    std::vector<std::unique_ptr<Statement>> body;
  };

  struct AssignmentStatement final : Statement {
    AssignmentStatement(std::string lhs, std::unique_ptr<Expression> rhs, SourceLocation location)
        : Statement(location), lhs(std::move(lhs)), rhs(std::move(rhs)) {}

    std::string lhs;
    std::unique_ptr<Expression> rhs;
  };

  struct Parameter {
    std::string name;
    std::string typeName;
    SourceLocation location;
  };

  struct Function {
    std::string name;
    std::string returnType;
    SourceLocation location;
    std::vector<Parameter> parameters;
    std::vector<std::unique_ptr<Statement>> body;
  };

  struct Module {
    std::vector<Function> functions;
  };

} // namespace noria::ast
