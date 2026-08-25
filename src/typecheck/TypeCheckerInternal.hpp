#pragma once

#include "noria/TypeChecker.hpp"

#include "../internal/AstVisitorAdapters.hpp"
#include "noria/AstVisitor.hpp"
#include "noria/Builtins.hpp"
#include "noria/HashTable.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace noria {

  class TypeChecker::Impl {
  public:
    Impl();
    Impl(const Impl& other);
    Impl& operator=(const Impl& other);

    void check(const ast::Module& module, const SymbolOrigins& symbolOrigins = {});
    void checkSpecializationFrontier(const ast::Module& module, std::size_t firstNewStruct,
                                     std::size_t firstNewFunction,
                                     const SymbolOrigins& symbolOrigins = {});
    void registerFunctionSpecialization(std::string mangledName, std::vector<Type> typeArgs);

    const std::vector<SpecializationRequest>& specializationRequests() const {
      return session_.specializationRequests;
    }
    const std::vector<StructSpecializationRequest>& structSpecializationRequests() const {
      return session_.structSpecializationRequests;
    }
    void clearSpecializationRequests() { session_.specializationRequests.clear(); }
    void clearStructSpecializationRequests() { session_.structSpecializationRequests.clear(); }
    std::vector<SpecializationRequest> takeSpecializationRequests();
    std::vector<StructSpecializationRequest> takeStructSpecializationRequests() const;

    class StatementVisitor final : public internal::StatementOnlyVisitor {
    public:
      using internal::StatementOnlyVisitor::visit;

      StatementVisitor(Impl& checker, Type expectedReturnType);

      bool returned() const { return returned_; }

      void visit(const ast::ReturnStatement& node) override;
      void visit(const ast::LetStatement& node) override;
      void visit(const ast::IfStatement& node) override;
      void visit(const ast::WhileStatement& node) override;
      void visit(const ast::AssignmentStatement& node) override;
      void visit(const ast::ExpressionStatement& node) override;

    private:
      Impl& checker_;
      Type expectedReturnType_;
      bool returned_ = false;
    };

    class ExpressionVisitor final : public internal::ExpressionOnlyVisitor {
    public:
      using internal::ExpressionOnlyVisitor::visit;

      ExpressionVisitor(Impl& checker, std::optional<Type> expectedType);

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

    private:
      Impl& checker_;
      std::optional<Type> expectedType_;
      Type result_;
    };

    struct PlaceInfo {
      std::string name;
      Type type;
    };

    class PlaceVisitor final : public ast::AstVisitor {
    public:
      explicit PlaceVisitor(Impl& checker);

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
      Impl& checker_;
      std::string name_;
      Type type_;
    };

    class TypeRelations {
    public:
      explicit TypeRelations(Impl& checker) : checker_(checker) {}

      void requireKnownType(const Type& type, SourceLocation location,
                            const std::unordered_set<std::string>* allowedTypeParams,
                            bool allowImplTags, bool allowInternalTypes) const;
      void requireRawPtrUsable(const Type& type, SourceLocation location,
                               bool allowInternalTypes) const;
      void requireImplTagUsable(const Type& type, SourceLocation location,
                                bool allowImplTags) const;
      void requireTypeParamKnown(const Type& type, SourceLocation location,
                                 const std::unordered_set<std::string>* allowedTypeParams) const;
      void requireArrayTypeKnown(const Type& type, SourceLocation location,
                                 const std::unordered_set<std::string>* allowedTypeParams,
                                 bool allowImplTags, bool allowInternalTypes) const;
      void requireStructTypeKnown(const Type& type, SourceLocation location,
                                  const std::unordered_set<std::string>* allowedTypeParams,
                                  bool allowInternalTypes) const;
      void unifyTypes(const Type& expected, const Type& actual,
                      std::unordered_map<std::string, Type>& bindings,
                      SourceLocation location) const;
      void bindTypeParam(const Type& expected, const Type& actual,
                         std::unordered_map<std::string, Type>& bindings,
                         SourceLocation location) const;
      void unifyArrayTypes(const Type& expected, const Type& actual,
                           std::unordered_map<std::string, Type>& bindings,
                           SourceLocation location) const;
      void unifyImplTagTypes(const Type& expected, const Type& actual,
                             SourceLocation location) const;
      void unifyStructTypes(const Type& expected, const Type& actual,
                            std::unordered_map<std::string, Type>& bindings,
                            SourceLocation location) const;
      void checkSpecializationConstraints(const std::string& templateName,
                                          const std::vector<Type>& typeArgs,
                                          SourceLocation location) const;
      void recordStructSpecialization(const std::string& templateName,
                                      const std::vector<Type>& typeArgs,
                                      SourceLocation location) const;
      bool isAssignable(Type expected, Type actual) const;

    private:
      Impl& checker_;
    };

    void requireKnownType(const Type& type, SourceLocation location,
                          const std::unordered_set<std::string>* allowedTypeParams = nullptr,
                          bool allowImplTags = false, bool allowInternalTypes = false) const {
      relations_.requireKnownType(type, location, allowedTypeParams, allowImplTags,
                                  allowInternalTypes);
    }
    void unifyTypes(const Type& expected, const Type& actual,
                    std::unordered_map<std::string, Type>& bindings,
                    SourceLocation location) const {
      relations_.unifyTypes(expected, actual, bindings, location);
    }
    bool isAssignable(Type expected, Type actual) const {
      return relations_.isAssignable(std::move(expected), std::move(actual));
    }

    struct StructFieldInfo {
      std::string name;
      Type type;
      std::size_t index;
      ast::FieldVisibility visibility = ast::FieldVisibility::Public;
    };

    struct StructInfo {
      std::vector<StructFieldInfo> fields;
      HashTable<std::string, std::size_t> fieldIndex;
    };

    void collectStructDecls(const ast::Module& module);
    void collectGenericStructDecl(const ast::StructDecl& decl, std::size_t moduleIndex);
    void collectConcreteStructDecl(const ast::StructDecl& decl);
    void validateConcreteStructFieldTypes(const ast::Module& module, std::size_t firstStruct = 0);
    bool allowsInternalStructTypes(const ast::StructDecl& decl) const;
    bool allowsInternalFunctionTypes(const ast::Function& function) const;
    void checkStructAcyclic(const std::string& structName, SourceLocation location) const;
    const StructInfo& lookupStruct(const std::string& name, SourceLocation location) const;
    StructInfo resolveStructInfo(const Type& structType, SourceLocation location) const;
    void checkSpecializationConstraints(const std::string& templateName,
                                        const std::vector<Type>& typeArgs,
                                        SourceLocation location) const {
      relations_.checkSpecializationConstraints(templateName, typeArgs, location);
    }
    void recordStructSpecialization(const std::string& templateName,
                                    const std::vector<Type>& typeArgs,
                                    SourceLocation location) const {
      relations_.recordStructSpecialization(templateName, typeArgs, location);
    }
    Type checkBinaryExpression(const ast::BinaryExpression& binary, const Type& left,
                               const Type& right) const;
    Type checkLogicalBinaryExpression(const ast::BinaryExpression& binary, const Type& left,
                                      const Type& right) const;
    Type checkAdditiveBinaryExpression(const ast::BinaryExpression& binary, const Type& left,
                                       const Type& right) const;
    Type checkIntegerBinaryExpression(const ast::BinaryExpression& binary, const Type& left,
                                      const Type& right) const;
    Type checkOrderedComparisonExpression(const ast::BinaryExpression& binary, const Type& left,
                                          const Type& right) const;
    Type checkEqualityExpression(const ast::BinaryExpression& binary, const Type& left,
                                 const Type& right) const;
    Type checkUnaryExpression(const ast::UnaryExpression& unary, const Type& operandType) const;
    Type checkNumericUnaryExpression(const ast::UnaryExpression& unary,
                                     const Type& operandType) const;
    Type checkBooleanUnaryExpression(const ast::UnaryExpression& unary,
                                     const Type& operandType) const;
    Type checkIntegerUnaryExpression(const ast::UnaryExpression& unary,
                                     const Type& operandType) const;
    Type checkGenericFunctionCall(const ast::CallExpression& call,
                                  const std::vector<std::size_t>& family,
                                  const std::optional<Type>& expectedType);
    Type checkConcreteFunctionCall(const ast::CallExpression& call,
                                   const FunctionSignature& signature);
    std::vector<Type> inferGenericCallTypeArgs(const ast::CallExpression& call,
                                               const ast::Function& signature,
                                               bool seedFromSpecializedCaller,
                                               const std::optional<Type>& expectedType,
                                               std::unordered_map<std::string, Type>& bindings);
    Type checkStructLiteral(const ast::StructLiteral& literal);
    Type checkGenericStructLiteral(const ast::StructLiteral& literal,
                                   const ast::StructDecl& templated);
    Type checkConcreteStructLiteral(const ast::StructLiteral& literal, const StructInfo& structInfo,
                                    std::vector<Type> typeArgs);
    std::vector<Type> inferStructLiteralTypeArgs(const ast::StructLiteral& literal,
                                                 const ast::StructDecl& templated);
    std::unordered_map<std::string, Type>
    checkStructLiteralFields(const ast::StructLiteral& literal, const StructInfo& structInfo);
    void requireStructLiteralComplete(const ast::StructLiteral& literal,
                                      const StructInfo& structInfo,
                                      const std::unordered_map<std::string, Type>& provided) const;

    void collectFunctionSignatures(const ast::Module& module);
    void collectGenericFunctionSignature(const ast::Function& function, std::size_t moduleIndex);
    void collectConcreteFunctionSignature(const ast::Function& function);
    void validateGenericFunctionFamily(std::string_view name,
                                       const std::vector<std::size_t>& family) const;
    void checkFunction(const ast::Function& function);
    bool checkStatements(const std::vector<std::unique_ptr<ast::Statement>>& statements,
                         Type expectedReturnType);
    bool checkStatement(const ast::Statement& statement, Type expectedReturnType);
    PlaceInfo checkPlace(const ast::Expression& place);
    Type checkRvalue(const ast::Expression& expression,
                     std::optional<Type> expectedType = std::nullopt);
    Type checkBuiltinCall(const ast::CallExpression& call, const BuiltinSignature& descriptor);
    void requireBuiltinCallable(const ast::CallExpression& call,
                                const BuiltinSignature& descriptor) const;
    Type checkLenBuiltin(const ast::CallExpression& call);
    Type checkRtSizeofBuiltin(const ast::CallExpression& call) const;
    Type checkRtHashBuiltin(const ast::CallExpression& call);
    Type checkRtLoadBuiltin(const ast::CallExpression& call, const BuiltinSignature& descriptor);
    Type checkRtStoreBuiltin(const ast::CallExpression& call, const BuiltinSignature& descriptor);
    Type checkAllArgumentsBuiltin(const ast::CallExpression& call,
                                  const BuiltinSignature& descriptor);
    Type checkDeclaredBuiltinArguments(const ast::CallExpression& call,
                                       const BuiltinSignature& descriptor);
    Type resolveWitnessType(SourceLocation location) const;
    bool isEnclosingFunctionSpecialized() const;
    const std::vector<Type>* enclosingFunctionSpecializationTypeArgs() const;
    void
    seedMatchingTypeParamsFromCaller(std::unordered_map<std::string, Type>& bindings,
                                     const std::vector<ast::TypeParameter>& calleeTypeParams) const;
    void seedUnboundTypeParamsFromCaller(std::unordered_map<std::string, Type>& bindings,
                                         const std::vector<ast::TypeParameter>& typeParams) const;
    void seedUnboundTypeParamsFromExpectedType(std::unordered_map<std::string, Type>& bindings,
                                               const Type& returnType,
                                               const std::optional<Type>& expectedType,
                                               SourceLocation location) const;
    using Scope = std::unordered_map<std::string, Type>;
    void pushScope();
    void popScope();
    bool declareLocal(const std::string& name, Type type);
    Type lookupLocal(const std::string& name, SourceLocation location) const;

    bool isStdlibOrigin(const std::string& modulePath) const;
    bool isInternalModuleOrigin(const std::string& modulePath) const;
    bool isStdlibContext() const;
    void requireFunctionCallable(const std::string& calleeName, SourceLocation location) const;
    std::string structOriginModule(const std::string& structName) const;
    std::string currentModuleOrigin() const;
    void requireFieldVisible(const std::string& structName, const StructFieldInfo& field,
                             SourceLocation location) const;
    const ast::Function& genericFunctionAt(std::size_t moduleIndex) const;
    const ast::StructDecl& genericStructAt(std::size_t moduleIndex) const;

    struct TypeEnvironment {
      const ast::Module* activeModule = nullptr;
      SymbolOrigins symbolOrigins;
      std::unordered_map<std::string, FunctionSignature> functions;
      std::unordered_map<std::string, std::vector<std::size_t>> genericFunctions;
      HashTable<std::string, std::size_t> genericStructs;
      std::unordered_map<std::string, StructInfo> structs;
    };

    struct TypeCheckSession {
      std::vector<SpecializationRequest> specializationRequests;
      mutable std::vector<StructSpecializationRequest> structSpecializationRequests;
      std::string currentFunctionName;
      std::unordered_map<std::string, std::vector<Type>> functionSpecializationTypeArgs;
      std::vector<Scope> scopes;
    };

    TypeEnvironment environment_;
    TypeCheckSession session_;
    TypeRelations relations_;
  };

} // namespace noria
