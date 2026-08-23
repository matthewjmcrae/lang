#pragma once

#include "noria/Ast.hpp"
#include "noria/AstVisitor.hpp"
#include "noria/Builtins.hpp"
#include "noria/Types.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace noria {

  struct FunctionSignature {
    Type returnType;
    std::vector<Type> parameterTypes;
  };

  class TypeChecker {
  public:
    void check(const ast::Module& module);

  private:
    class StatementVisitor final : public ast::AstVisitor {
    public:
      StatementVisitor(TypeChecker& checker, Type expectedReturnType);

      bool returned() const { return returned_; }

      void visit(const ast::ReturnStatement& node) override;
      void visit(const ast::LetStatement& node) override;
      void visit(const ast::IfStatement& node) override;
      void visit(const ast::WhileStatement& node) override;
      void visit(const ast::AssignmentStatement& node) override;
      void visit(const ast::ExpressionStatement& node) override;

      void visit(const ast::IntegerLiteral& node) override;
      void visit(const ast::FloatLiteral& node) override;
      void visit(const ast::StringLiteral& node) override;
      void visit(const ast::BoolLiteral& node) override;
      void visit(const ast::UnaryExpression& node) override;
      void visit(const ast::CastExpression& node) override;
      void visit(const ast::BinaryExpression& node) override;
      void visit(const ast::IdentifierExpression& node) override;
      void visit(const ast::CallExpression& node) override;
      void visit(const ast::IndexExpression& node) override;

    private:
      TypeChecker& checker_;
      Type expectedReturnType_;
      bool returned_ = false;
    };

    class ExpressionVisitor final : public ast::AstVisitor {
    public:
      explicit ExpressionVisitor(TypeChecker& checker);

      Type result() const { return result_; }

      void visit(const ast::IntegerLiteral& node) override;
      void visit(const ast::FloatLiteral& node) override;
      void visit(const ast::StringLiteral& node) override;
      void visit(const ast::BoolLiteral& node) override;
      void visit(const ast::UnaryExpression& node) override;
      void visit(const ast::CastExpression& node) override;
      void visit(const ast::BinaryExpression& node) override;
      void visit(const ast::IdentifierExpression& node) override;
      void visit(const ast::CallExpression& node) override;
      void visit(const ast::IndexExpression& node) override;

      void visit(const ast::ReturnStatement& node) override;
      void visit(const ast::LetStatement& node) override;
      void visit(const ast::IfStatement& node) override;
      void visit(const ast::WhileStatement& node) override;
      void visit(const ast::AssignmentStatement& node) override;
      void visit(const ast::ExpressionStatement& node) override;

    private:
      TypeChecker& checker_;
      Type result_;
    };

    struct PlaceInfo {
      std::string name;
      Type type;
    };

    class PlaceVisitor final : public ast::AstVisitor {
    public:
      const std::string& name() const { return name_; }

      void visit(const ast::IdentifierExpression& node) override;

      void visit(const ast::IntegerLiteral& node) override;
      void visit(const ast::FloatLiteral& node) override;
      void visit(const ast::StringLiteral& node) override;
      void visit(const ast::BoolLiteral& node) override;
      void visit(const ast::UnaryExpression& node) override;
      void visit(const ast::CastExpression& node) override;
      void visit(const ast::BinaryExpression& node) override;
      void visit(const ast::CallExpression& node) override;
      void visit(const ast::IndexExpression& node) override;

      void visit(const ast::ReturnStatement& node) override;
      void visit(const ast::LetStatement& node) override;
      void visit(const ast::IfStatement& node) override;
      void visit(const ast::WhileStatement& node) override;
      void visit(const ast::AssignmentStatement& node) override;
      void visit(const ast::ExpressionStatement& node) override;

    private:
      std::string name_;
    };

    class CallExpressionProbe final : public ast::AstVisitor {
    public:
      bool isCallExpression() const { return isCallExpression_; }

      void visit(const ast::CallExpression& node) override;

      void visit(const ast::IntegerLiteral& node) override;
      void visit(const ast::FloatLiteral& node) override;
      void visit(const ast::StringLiteral& node) override;
      void visit(const ast::BoolLiteral& node) override;
      void visit(const ast::UnaryExpression& node) override;
      void visit(const ast::CastExpression& node) override;
      void visit(const ast::BinaryExpression& node) override;
      void visit(const ast::IdentifierExpression& node) override;
      void visit(const ast::IndexExpression& node) override;

      void visit(const ast::ReturnStatement& node) override;
      void visit(const ast::LetStatement& node) override;
      void visit(const ast::IfStatement& node) override;
      void visit(const ast::WhileStatement& node) override;
      void visit(const ast::AssignmentStatement& node) override;
      void visit(const ast::ExpressionStatement& node) override;

    private:
      bool isCallExpression_ = false;
    };

    void requireKnownType(const Type& type, SourceLocation location) const;
    bool isAssignable(Type expected, Type actual) const;

    void collectFunctionSignatures(const ast::Module& module);
    void checkFunction(const ast::Function& function);
    bool checkStatements(const std::vector<std::unique_ptr<ast::Statement>>& statements,
                         Type expectedReturnType);
    bool checkStatement(const ast::Statement& statement, Type expectedReturnType);
    PlaceInfo checkPlace(const ast::Expression& place);
    Type checkRvalue(const ast::Expression& expression);
    Type checkBuiltinCall(const ast::CallExpression& call, const BuiltinSignature& descriptor);
    using Scope = std::unordered_map<std::string, Type>;
    void pushScope();
    void popScope();
    bool declareLocal(const std::string& name, Type type);
    Type lookupLocal(const std::string& name, SourceLocation location) const;

    std::unordered_map<std::string, FunctionSignature> functions_;
    std::vector<Scope> scopes_;
  };

} // namespace noria
