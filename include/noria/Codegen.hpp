#pragma once

#include "noria/Ast.hpp"
#include "noria/AstVisitor.hpp"
#include "noria/Types.hpp"

#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>

namespace noria {

  class LlvmIrTextGenerator {
  public:
    std::string generate(const ast::Module& module) const;

  private:
    struct Value {
      std::string text;
      Type type;
    };

    struct LocalBinding {
      std::string slot;
      Type type;
    };

    struct FunctionBinding {
      Type returnType;
      std::vector<Type> parameterTypes;
    };

    using Scope = std::unordered_map<std::string, LocalBinding>;

    class StatementVisitor final : public ast::AstVisitor {
    public:
      StatementVisitor(const LlvmIrTextGenerator& generator, std::ostringstream& out,
                       int& nextTemporary, int& nextLabel, Type expectedReturnType,
                       std::vector<Scope>& scopes);

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

    private:
      const LlvmIrTextGenerator& generator_;
      std::ostringstream& out_;
      int& nextTemporary_;
      int& nextLabel_;
      Type expectedReturnType_;
      std::vector<Scope>& scopes_;
      bool returned_ = false;
    };

    class ExpressionVisitor final : public ast::AstVisitor {
    public:
      ExpressionVisitor(const LlvmIrTextGenerator& generator, std::ostringstream& out,
                        int& nextTemporary, int& nextLabel, const std::vector<Scope>& scopes);

      Value result() const { return result_; }

      void visit(const ast::IntegerLiteral& node) override;
      void visit(const ast::FloatLiteral& node) override;
      void visit(const ast::StringLiteral& node) override;
      void visit(const ast::BoolLiteral& node) override;
      void visit(const ast::UnaryExpression& node) override;
      void visit(const ast::CastExpression& node) override;
      void visit(const ast::BinaryExpression& node) override;
      void visit(const ast::IdentifierExpression& node) override;
      void visit(const ast::CallExpression& node) override;

      void visit(const ast::ReturnStatement& node) override;
      void visit(const ast::LetStatement& node) override;
      void visit(const ast::IfStatement& node) override;
      void visit(const ast::WhileStatement& node) override;
      void visit(const ast::AssignmentStatement& node) override;
      void visit(const ast::ExpressionStatement& node) override;

    private:
      const LlvmIrTextGenerator& generator_;
      std::ostringstream& out_;
      int& nextTemporary_;
      int& nextLabel_;
      const std::vector<Scope>& scopes_;
      Value result_{};
    };

    class ComparisonProbe final : public ast::AstVisitor {
    public:
      const ast::BinaryExpression* comparison() const { return comparison_; }

      void visit(const ast::BinaryExpression& node) override;

      void visit(const ast::IntegerLiteral& node) override;
      void visit(const ast::FloatLiteral& node) override;
      void visit(const ast::StringLiteral& node) override;
      void visit(const ast::BoolLiteral& node) override;
      void visit(const ast::UnaryExpression& node) override;
      void visit(const ast::CastExpression& node) override;
      void visit(const ast::IdentifierExpression& node) override;
      void visit(const ast::CallExpression& node) override;

      void visit(const ast::ReturnStatement& node) override;
      void visit(const ast::LetStatement& node) override;
      void visit(const ast::IfStatement& node) override;
      void visit(const ast::WhileStatement& node) override;
      void visit(const ast::AssignmentStatement& node) override;
      void visit(const ast::ExpressionStatement& node) override;

    private:
      const ast::BinaryExpression* comparison_ = nullptr;
    };

    std::string generateFunction(const ast::Function& function) const;
    bool generateStatement(const ast::Statement& statement, std::ostringstream& out,
                           int& nextTemporary, int& nextLabel, Type expectedReturnType,
                           std::vector<Scope>& scopes) const;
    bool generateStatements(const std::vector<std::unique_ptr<ast::Statement>>& statements,
                            std::ostringstream& out, int& nextTemporary, int& nextLabel,
                            Type expectedReturnType, std::vector<Scope>& scopes) const;
    std::string generateCondition(const ast::Expression& expression, std::ostringstream& out,
                                  int& nextTemporary, int& nextLabel,
                                  const std::vector<Scope>& scopes) const;
    Value generateExpression(const ast::Expression& expression, std::ostringstream& out,
                             int& nextTemporary, int& nextLabel,
                             const std::vector<Scope>& scopes) const;

    Value generateBinaryExpression(const ast::BinaryExpression& binary, std::ostringstream& out,
                                   int& nextTemporary, int& nextLabel,
                                   const std::vector<Scope>& scopes) const;
    Value generateStringLiteral(const ast::StringLiteral& literal, std::ostringstream& out,
                                int& nextTemporary) const;
    Value generateCastExpression(const ast::CastExpression& cast, std::ostringstream& out,
                                 int& nextTemporary, int& nextLabel,
                                 const std::vector<Scope>& scopes) const;
    std::optional<Value> tryGenerateBuiltinCall(const ast::CallExpression& call,
                                                std::ostringstream& out, int& nextTemporary,
                                                int& nextLabel,
                                                const std::vector<Scope>& scopes) const;

    std::string defaultIrValue(const Type& type) const;
    std::string modulePreamble() const;
    bool declareLocal(std::vector<Scope>& scopes, const std::string& name,
                      LocalBinding binding) const;
    const LocalBinding& lookupLocal(const std::vector<Scope>& scopes,
                                    const std::string& name) const;
    void collectFunctionBindings(const ast::Module& module) const;

    mutable std::unordered_map<std::string, FunctionBinding> functions_;
    mutable std::ostringstream moduleGlobals_;
    mutable int nextStringGlobal_ = 0;
  };

} // namespace noria
