#pragma once

#include "noria/Ast.hpp"
#include "noria/AstVisitor.hpp"
#include "noria/Builtins.hpp"
#include "noria/ModuleResolver.hpp"
#include "noria/Monomorphize.hpp"
#include "noria/Types.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace noria {

  struct FunctionSignature {
    Type returnType;
    std::vector<Type> parameterTypes;
  };

  class TypeChecker {
  public:
    void check(const ast::Module& module, const SymbolOrigins& symbolOrigins = {});

    const std::vector<SpecializationRequest>& specializationRequests() const {
      return specializationRequests_;
    }

    const std::vector<StructSpecializationRequest>& structSpecializationRequests() const {
      return structSpecializationRequests_;
    }

    void clearSpecializationRequests() { specializationRequests_.clear(); }

    void clearStructSpecializationRequests() { structSpecializationRequests_.clear(); }

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
      void visit(const ast::ArrayLiteral& node) override;
      void visit(const ast::IndexExpression& node) override;
      void visit(const ast::StructLiteral& node) override;
      void visit(const ast::FieldAccessExpression& node) override;

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
      void visit(const ast::ArrayLiteral& node) override;
      void visit(const ast::IndexExpression& node) override;
      void visit(const ast::StructLiteral& node) override;
      void visit(const ast::FieldAccessExpression& node) override;

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
      explicit PlaceVisitor(TypeChecker& checker);

      const std::string& name() const { return name_; }
      Type type() const { return type_; }

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
      void visit(const ast::StructLiteral& node) override;
      void visit(const ast::FieldAccessExpression& node) override;

      void visit(const ast::ReturnStatement& node) override;
      void visit(const ast::LetStatement& node) override;
      void visit(const ast::IfStatement& node) override;
      void visit(const ast::WhileStatement& node) override;
      void visit(const ast::AssignmentStatement& node) override;
      void visit(const ast::ExpressionStatement& node) override;

    private:
      TypeChecker& checker_;
      std::string name_;
      Type type_;
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
      void visit(const ast::ArrayLiteral& node) override;
      void visit(const ast::IndexExpression& node) override;
      void visit(const ast::StructLiteral& node) override;
      void visit(const ast::FieldAccessExpression& node) override;

      void visit(const ast::ReturnStatement& node) override;
      void visit(const ast::LetStatement& node) override;
      void visit(const ast::IfStatement& node) override;
      void visit(const ast::WhileStatement& node) override;
      void visit(const ast::AssignmentStatement& node) override;
      void visit(const ast::ExpressionStatement& node) override;

    private:
      bool isCallExpression_ = false;
    };

    void requireKnownType(const Type& type, SourceLocation location,
                          const std::unordered_set<std::string>* allowedTypeParams = nullptr,
                          bool allowImplTags = false, bool allowInternalTypes = false) const;
    void unifyTypes(const Type& expected, const Type& actual,
                    std::unordered_map<std::string, Type>& bindings, SourceLocation location) const;
    bool isAssignable(Type expected, Type actual) const;

    struct StructFieldInfo {
      std::string name;
      Type type;
      std::size_t index;
    };

    struct StructInfo {
      std::vector<StructFieldInfo> fields;
      std::unordered_map<std::string, std::size_t> fieldIndex;
    };

    void collectStructDecls(const ast::Module& module);
    void checkStructAcyclic(const std::string& structName, SourceLocation location) const;
    const StructInfo& lookupStruct(const std::string& name, SourceLocation location) const;
    StructInfo resolveStructInfo(const Type& structType, SourceLocation location) const;
    void checkSpecializationConstraints(const std::string& templateName,
                                        const std::vector<Type>& typeArgs,
                                        SourceLocation location) const;
    void recordStructSpecialization(const std::string& templateName,
                                    const std::vector<Type>& typeArgs,
                                    SourceLocation location) const;

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

    bool isStdlibOrigin(const std::string& modulePath) const;
    bool isStdlibContext() const;

    SymbolOrigins symbolOrigins_;
    std::unordered_map<std::string, FunctionSignature> functions_;
    std::unordered_map<std::string, std::vector<const ast::Function*>> genericFunctions_;
    std::unordered_map<std::string, const ast::StructDecl*> genericStructs_;
    std::vector<SpecializationRequest> specializationRequests_;
    mutable std::vector<StructSpecializationRequest> structSpecializationRequests_;
    std::string currentFunctionName_;
    std::unordered_map<std::string, StructInfo> structs_;
    std::vector<Scope> scopes_;
  };

} // namespace noria
