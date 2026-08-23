#pragma once

#include "noria/Ast.hpp"
#include "noria/AstVisitor.hpp"
#include "noria/IrEmitter.hpp"
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

    struct CodegenContext {
      std::unordered_map<std::string, FunctionBinding> functions;
      std::ostringstream globals;
      int nextStringGlobal = 0;
    };

    using Scope = std::unordered_map<std::string, LocalBinding>;

    class StatementVisitor final : public ast::AstVisitor {
    public:
      StatementVisitor(const LlvmIrTextGenerator& generator, IrEmitter& emitter,
                       CodegenContext& context, Type expectedReturnType,
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
      void visit(const ast::ArrayLiteral& node) override;
      void visit(const ast::IndexExpression& node) override;

    private:
      const LlvmIrTextGenerator& generator_;
      IrEmitter& emitter_;
      CodegenContext& context_;
      Type expectedReturnType_;
      std::vector<Scope>& scopes_;
      bool returned_ = false;
    };

    class ExpressionVisitor final : public ast::AstVisitor {
    public:
      ExpressionVisitor(const LlvmIrTextGenerator& generator, IrEmitter& emitter,
                        CodegenContext& context, const std::vector<Scope>& scopes);

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
      void visit(const ast::ArrayLiteral& node) override;
      void visit(const ast::IndexExpression& node) override;

      void visit(const ast::ReturnStatement& node) override;
      void visit(const ast::LetStatement& node) override;
      void visit(const ast::IfStatement& node) override;
      void visit(const ast::WhileStatement& node) override;
      void visit(const ast::AssignmentStatement& node) override;
      void visit(const ast::ExpressionStatement& node) override;

    private:
      const LlvmIrTextGenerator& generator_;
      IrEmitter& emitter_;
      CodegenContext& context_;
      const std::vector<Scope>& scopes_;
      Value result_{};
    };

    class PlaceVisitor final : public ast::AstVisitor {
    public:
      PlaceVisitor(const LlvmIrTextGenerator& generator, IrEmitter& emitter,
                   CodegenContext& context, const std::vector<Scope>& scopes);

      LocalBinding result() const { return result_; }

      void visit(const ast::IdentifierExpression& node) override;

      void visit(const ast::IntegerLiteral& node) override;
      void visit(const ast::FloatLiteral& node) override;
      void visit(const ast::StringLiteral& node) override;
      void visit(const ast::BoolLiteral& node) override;
      void visit(const ast::UnaryExpression& node) override;
      void visit(const ast::CastExpression& node) override;
      void visit(const ast::BinaryExpression& node) override;
      void visit(const ast::CallExpression& node) override;
      void visit(const ast::ArrayLiteral& node) override;
      void visit(const ast::IndexExpression& node) override;

      void visit(const ast::ReturnStatement& node) override;
      void visit(const ast::LetStatement& node) override;
      void visit(const ast::IfStatement& node) override;
      void visit(const ast::WhileStatement& node) override;
      void visit(const ast::AssignmentStatement& node) override;
      void visit(const ast::ExpressionStatement& node) override;

    private:
      const LlvmIrTextGenerator& generator_;
      IrEmitter& emitter_;
      CodegenContext& context_;
      const std::vector<Scope>& scopes_;
      LocalBinding result_{};
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
      void visit(const ast::ArrayLiteral& node) override;
      void visit(const ast::IndexExpression& node) override;

      void visit(const ast::ReturnStatement& node) override;
      void visit(const ast::LetStatement& node) override;
      void visit(const ast::IfStatement& node) override;
      void visit(const ast::WhileStatement& node) override;
      void visit(const ast::AssignmentStatement& node) override;
      void visit(const ast::ExpressionStatement& node) override;

    private:
      const ast::BinaryExpression* comparison_ = nullptr;
    };

    std::string generateFunction(const ast::Function& function, CodegenContext& context) const;
    bool generateStatement(const ast::Statement& statement, IrEmitter& emitter,
                           CodegenContext& context, Type expectedReturnType,
                           std::vector<Scope>& scopes) const;
    bool generateStatements(const std::vector<std::unique_ptr<ast::Statement>>& statements,
                            IrEmitter& emitter, CodegenContext& context, Type expectedReturnType,
                            std::vector<Scope>& scopes) const;
    std::string generateCondition(const ast::Expression& expression, IrEmitter& emitter,
                                  CodegenContext& context, const std::vector<Scope>& scopes) const;
    LocalBinding generatePlace(const ast::Expression& place, IrEmitter& emitter,
                               CodegenContext& context, const std::vector<Scope>& scopes) const;
    std::string emitArrayElementPointer(const Value& base, const Value& indexValue,
                                        const Type& elementType, IrEmitter& emitter) const;
    Value generateRvalue(const ast::Expression& expression, IrEmitter& emitter,
                         CodegenContext& context, const std::vector<Scope>& scopes) const;

    Value generateBinaryExpression(const ast::BinaryExpression& binary, IrEmitter& emitter,
                                   CodegenContext& context, const std::vector<Scope>& scopes) const;
    Value generateStringLiteral(const ast::StringLiteral& literal, IrEmitter& emitter,
                                CodegenContext& context) const;
    Value generateCastExpression(const ast::CastExpression& cast, IrEmitter& emitter,
                                 CodegenContext& context, const std::vector<Scope>& scopes) const;
    Value generateArrayLiteral(const ast::ArrayLiteral& literal, IrEmitter& emitter,
                               CodegenContext& context, const std::vector<Scope>& scopes) const;
    Value generateIndexExpression(const ast::IndexExpression& index, IrEmitter& emitter,
                                  CodegenContext& context, const std::vector<Scope>& scopes) const;
    std::optional<Value> tryGenerateBuiltinCall(const ast::CallExpression& call, IrEmitter& emitter,
                                                CodegenContext& context,
                                                const std::vector<Scope>& scopes) const;

    std::string defaultIrValue(const Type& type) const;
    std::string modulePreamble() const;
    bool declareLocal(std::vector<Scope>& scopes, const std::string& name,
                      LocalBinding binding) const;
    const LocalBinding& lookupLocal(const std::vector<Scope>& scopes,
                                    const std::string& name) const;
    std::unordered_map<std::string, FunctionBinding>
    collectFunctionBindings(const ast::Module& module) const;
  };

} // namespace noria
