#pragma once

#include "noria/TypeChecker.hpp"

#include "../internal/AstVisitorAdapters.hpp"
#include "noria/AstVisitor.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace noria {

  class TypeChecker::TypeCheckerState {
  public:
    explicit TypeCheckerState(TypeChecker& checker) : checker_(&checker) {}
    virtual ~TypeCheckerState() = default;
    void rebind(TypeChecker& checker) noexcept { checker_ = &checker; }

  protected:
    TypeChecker& checker() { return *checker_; }
    const TypeChecker& checker() const { return *checker_; }

    TypeEnvironment& environment() { return checker().environment_; }
    const TypeEnvironment& environment() const { return checker().environment_; }
    TypeCheckSession& session() { return checker().session_; }
    const TypeCheckSession& session() const { return checker().session_; }
    TypeRelations& relations() { return checker().relations_; }
    const TypeRelations& relations() const { return checker().relations_; }
    std::unordered_set<std::string>& pendingReturnTypeFunctions() {
      return checker().pendingReturnTypeFunctions_;
    }
    const std::unordered_set<std::string>& pendingReturnTypeFunctions() const {
      return checker().pendingReturnTypeFunctions_;
    }
    bool& inferringReturnTypes() { return checker().inferringReturnTypes_; }
    bool inferringReturnTypes() const { return checker().inferringReturnTypes_; }

    void checkFunction(const ast::Function& function) { checker().checkFunction(function); }
    void inferFunctionReturnTypes(ast::Module& module) {
      checker().inferFunctionReturnTypes(module);
    }
    bool checkStatements(const std::vector<std::unique_ptr<ast::Statement>>& statements,
                         Type expected) {
      return checker().checkStatements(statements, expected);
    }
    bool checkStatement(const ast::Statement& statement, Type expected) {
      return checker().checkStatement(statement, expected);
    }
    PlaceInfo checkPlace(const ast::Expression& expression) {
      return checker().checkPlace(expression);
    }
    Type checkRvalue(const ast::Expression& expression,
                     std::optional<Type> expectedType = std::nullopt) {
      return checker().checkRvalue(expression, std::move(expectedType));
    }
    void collectStructDecls(const ast::Module& module) { checker().collectStructDecls(module); }
    void collectFunctionSignatures(const ast::Module& module) {
      checker().collectFunctionSignatures(module);
    }
    Type checkBuiltinCall(const ast::CallExpression& call, const BuiltinSignature& signature) {
      return checker().checkBuiltinCall(call, signature);
    }
    Type checkGenericFunctionCall(const ast::CallExpression& call,
                                  const std::vector<std::size_t>& family,
                                  const std::optional<Type>& expected) {
      return checker().checkGenericFunctionCall(call, family, expected);
    }
    Type checkConcreteFunctionCall(const ast::CallExpression& call,
                                   const FunctionSignature& signature) {
      return checker().checkConcreteFunctionCall(call, signature);
    }
    Type checkStructLiteral(const ast::StructLiteral& literal) {
      return checker().checkStructLiteral(literal);
    }
    Type checkBinaryExpression(const ast::BinaryExpression& binary, const Type& left,
                               const Type& right) const {
      return checker().checkBinaryExpression(binary, left, right);
    }
    Type checkUnaryExpression(const ast::UnaryExpression& unary, const Type& operand) const {
      return checker().checkUnaryExpression(unary, operand);
    }
    void requireKnownType(const Type& type, SourceLocation location,
                          const std::unordered_set<std::string>* params = nullptr,
                          bool allowImplTags = false, bool allowInternalTypes = false) const {
      checker().requireKnownType(type, location, params, allowImplTags, allowInternalTypes);
    }
    void unifyTypes(const Type& expected, const Type& actual,
                    std::unordered_map<std::string, Type>& bindings,
                    SourceLocation location) const {
      checker().unifyTypes(expected, actual, bindings, location);
    }
    bool isAssignable(Type expected, Type actual) const {
      return checker().isAssignable(std::move(expected), std::move(actual));
    }
    void checkSpecializationConstraints(const std::string& name, const std::vector<Type>& args,
                                        SourceLocation location) const {
      checker().checkSpecializationConstraints(name, args, location);
    }
    void recordStructSpecialization(const std::string& name, const std::vector<Type>& args,
                                    SourceLocation location) const {
      checker().recordStructSpecialization(name, args, location);
    }
    std::optional<StandardContainer> standardContainerFor(const Type& type) const {
      return checker().standardContainerFor(type);
    }
    Type canonicalStructType(const Type& type) const {
      return checker().canonicalStructType(type);
    }
    void recordImplicitContainerOperation(StandardContainer container, ContainerOperation operation,
                                          const std::vector<Type>& typeArgs,
                                          SourceLocation location) {
      checker().recordImplicitContainerOperation(container, operation, typeArgs, location);
    }
    void requireDefaultInitializable(const Type& type, SourceLocation location) {
      checker().requireDefaultInitializable(type, location);
    }
    void requireContainerOwnershipOps(const Type& type, SourceLocation location) {
      checker().requireContainerOwnershipOps(type, location);
    }
    void pushScope() { checker().pushScope(); }
    void popScope() { checker().popScope(); }
    bool declareLocal(const std::string& name, Type type) {
      return checker().declareLocal(name, std::move(type));
    }
    Type lookupLocal(const std::string& name, SourceLocation location) const {
      return checker().lookupLocal(name, location);
    }
    bool isStdlibOrigin(const std::string& modulePath) const {
      return checker().isStdlibOrigin(modulePath);
    }
    bool isInternalModuleOrigin(const std::string& modulePath) const {
      return checker().isInternalModuleOrigin(modulePath);
    }
    bool isStdlibContext() const { return checker().isStdlibContext(); }
    void requireFunctionCallable(const std::string& name, SourceLocation location) const {
      checker().requireFunctionCallable(name, location);
    }
    std::string structOriginModule(const std::string& structName) const {
      return checker().structOriginModule(structName);
    }
    std::string currentModuleOrigin() const { return checker().currentModuleOrigin(); }
    void requireFieldVisible(const std::string& structName, const StructFieldInfo& field,
                             SourceLocation location) const {
      checker().requireFieldVisible(structName, field, location);
    }
    const ast::Function& genericFunctionAt(std::size_t index) const {
      return checker().genericFunctionAt(index);
    }
    const ast::StructDecl& genericStructAt(std::size_t index) const {
      return checker().genericStructAt(index);
    }
    StructInfo resolveStructInfo(const Type& type, SourceLocation location) const {
      return checker().resolveStructInfo(type, location);
    }
    const StructInfo& lookupStruct(const std::string& name, SourceLocation location) const {
      return checker().lookupStruct(name, location);
    }
    void requireDefinableFunctionName(const ast::Function& function) const {
      checker().requireDefinableFunctionName(function);
    }
    void collectGenericFunctionSignature(const ast::Function& function, std::size_t index) {
      checker().collectGenericFunctionSignature(function, index);
    }
    void collectConcreteFunctionSignature(const ast::Function& function) {
      checker().collectConcreteFunctionSignature(function);
    }
    void collectGenericStructDecl(const ast::StructDecl& decl, std::size_t index) {
      checker().collectGenericStructDecl(decl, index);
    }
    void collectConcreteStructDecl(const ast::StructDecl& decl) {
      checker().collectConcreteStructDecl(decl);
    }
    bool allowsInternalStructTypes(const ast::StructDecl& decl) const {
      return checker().allowsInternalStructTypes(decl);
    }
    bool allowsInternalFunctionTypes(const ast::Function& function) const {
      return checker().allowsInternalFunctionTypes(function);
    }
    void checkStructAcyclic(const std::string& name, SourceLocation location) const {
      checker().checkStructAcyclic(name, location);
    }
    void validateConcreteStructFieldTypes(const ast::Module& module, std::size_t firstStruct = 0) {
      checker().validateConcreteStructFieldTypes(module, firstStruct);
    }
    void validateGenericFunctionFamily(std::string_view name,
                                       const std::vector<std::size_t>& family) const {
      checker().validateGenericFunctionFamily(name, family);
    }

  private:
    TypeChecker* checker_;
  };

  class TypeChecker::DriverState final : public TypeCheckerState {
  public:
    explicit DriverState(TypeChecker& checker) : TypeCheckerState(checker) {}
    void check(ast::Module&, const SymbolOrigins&);
    void checkSpecializationFrontier(const ast::Module&, std::size_t, std::size_t,
                                     const SymbolOrigins&);
    void checkFunction(const ast::Function&);
    void inferFunctionReturnTypes(ast::Module&);
    std::optional<Type> inferFunctionReturnType(const ast::Function&);
    void inferReturnTypesInStatements(const std::vector<std::unique_ptr<ast::Statement>>&,
                                      ReturnInferenceResult&);
    void mergeInferredReturnType(ReturnInferenceResult&, Type, SourceLocation);
  };

  class TypeChecker::CallsState final : public TypeCheckerState {
  public:
    explicit CallsState(TypeChecker& checker) : TypeCheckerState(checker) {}
    Type checkBuiltinCall(const ast::CallExpression&, const BuiltinSignature&);
    void requireBuiltinCallable(const ast::CallExpression&, const BuiltinSignature&) const;
    Type checkLenBuiltin(const ast::CallExpression&);
    Type checkRtSizeofBuiltin(const ast::CallExpression&) const;
    Type checkRtHashBuiltin(const ast::CallExpression&);
    Type checkRtLoadBuiltin(const ast::CallExpression&, const BuiltinSignature&);
    Type checkRtStoreBuiltin(const ast::CallExpression&, const BuiltinSignature&);
    Type checkRtDropBuiltin(const ast::CallExpression&, const BuiltinSignature&);
    Type checkAllArgumentsBuiltin(const ast::CallExpression&, const BuiltinSignature&);
    Type checkDeclaredBuiltinArguments(const ast::CallExpression&, const BuiltinSignature&);
    Type checkGenericFunctionCall(const ast::CallExpression&, const std::vector<std::size_t>&,
                                  const std::optional<Type>&);
    Type checkConcreteFunctionCall(const ast::CallExpression&, const FunctionSignature&);
    std::vector<Type> inferGenericCallTypeArgs(const ast::CallExpression&, const ast::Function&,
                                               bool, const std::optional<Type>&,
                                               std::unordered_map<std::string, Type>&);
    Type resolveWitnessType(SourceLocation) const;
    bool isEnclosingFunctionSpecialized() const;
    const std::vector<Type>* enclosingFunctionSpecializationTypeArgs() const;
    void seedMatchingTypeParamsFromCaller(std::unordered_map<std::string, Type>&,
                                          const std::vector<ast::TypeParameter>&) const;
    void seedUnboundTypeParamsFromCaller(std::unordered_map<std::string, Type>&,
                                         const std::vector<ast::TypeParameter>&) const;
    void seedUnboundTypeParamsFromExpectedType(std::unordered_map<std::string, Type>&, const Type&,
                                               const std::optional<Type>&, SourceLocation) const;
  };

  class TypeChecker::DeclarationsState final : public TypeCheckerState {
  public:
    explicit DeclarationsState(TypeChecker& checker) : TypeCheckerState(checker) {}
    void collectFunctionSignatures(const ast::Module&);
    void collectGenericFunctionSignature(const ast::Function&, std::size_t);
    void collectConcreteFunctionSignature(const ast::Function&);
    void requireDefinableFunctionName(const ast::Function&) const;
    void validateGenericFunctionFamily(std::string_view, const std::vector<std::size_t>&) const;
    bool allowsInternalFunctionTypes(const ast::Function&) const;
    bool isStdlibOrigin(const std::string&) const;
    bool isInternalModuleOrigin(const std::string&) const;
    bool isStdlibContext() const;
    void requireFunctionCallable(const std::string&, SourceLocation) const;
    const ast::Function& genericFunctionAt(std::size_t) const;
    const ast::StructDecl& genericStructAt(std::size_t) const;
  };

  class TypeChecker::ExpressionsState final : public TypeCheckerState {
  public:
    explicit ExpressionsState(TypeChecker& checker) : TypeCheckerState(checker) {}
    Type checkRvalue(const ast::Expression&, std::optional<Type> expectedType = std::nullopt);
    Type checkBinaryExpression(const ast::BinaryExpression&, const Type&, const Type&) const;
    void rejectStaticallyInvalidIntegerOperation(const ast::BinaryExpression&, const Type&) const;
    Type checkLogicalBinaryExpression(const ast::BinaryExpression&, const Type&, const Type&) const;
    Type checkAdditiveBinaryExpression(const ast::BinaryExpression&, const Type&,
                                       const Type&) const;
    std::optional<Type> sequenceElementType(const Type&) const;
    bool supportsCollectionAddition(const Type&) const;
    Type checkIntegerBinaryExpression(const ast::BinaryExpression&, const Type&, const Type&) const;
    Type checkOrderedComparisonExpression(const ast::BinaryExpression&, const Type&,
                                          const Type&) const;
    Type checkEqualityExpression(const ast::BinaryExpression&, const Type&, const Type&) const;
    Type checkUnaryExpression(const ast::UnaryExpression&, const Type&) const;
    Type checkNumericUnaryExpression(const ast::UnaryExpression&, const Type&) const;
    Type checkBooleanUnaryExpression(const ast::UnaryExpression&, const Type&) const;
    Type checkIntegerUnaryExpression(const ast::UnaryExpression&, const Type&) const;

  private:
    class ExpressionVisitor final : public internal::ExpressionOnlyVisitor {
    public:
      using internal::ExpressionOnlyVisitor::visit;
      ExpressionVisitor(ExpressionsState&, std::optional<Type>);
      Type result() const { return result_; }
      void visit(const ast::IntegerLiteral&) override;
      void visit(const ast::FloatLiteral&) override;
      void visit(const ast::StringLiteral&) override;
      void visit(const ast::BoolLiteral&) override;
      void visit(const ast::UnaryExpression&) override;
      void visit(const ast::CastExpression&) override;
      void visit(const ast::BinaryExpression&) override;
      void visit(const ast::IdentifierExpression&) override;
      void visit(const ast::CallExpression&) override;
      void visit(const ast::ArrayLiteral&) override;
      void visit(const ast::IndexExpression&) override;
      void visit(const ast::StructLiteral&) override;
      void visit(const ast::FieldAccessExpression&) override;

    private:
      ExpressionsState& state_;
      std::optional<Type> expectedType_;
      Type result_;
    };
  };

  class TypeChecker::PlacesState final : public TypeCheckerState {
  public:
    explicit PlacesState(TypeChecker& checker) : TypeCheckerState(checker) {}
    PlaceInfo checkPlace(const ast::Expression&);

  private:
    class PlaceVisitor final : public ast::AstVisitor {
    public:
      explicit PlaceVisitor(PlacesState&);
      const std::string& name() const { return name_; }
      Type type() const { return type_; }
      void visit(const ast::IdentifierExpression&) override;
      void visit(const ast::IntegerLiteral&) override;
      void visit(const ast::FloatLiteral&) override;
      void visit(const ast::StringLiteral&) override;
      void visit(const ast::BoolLiteral&) override;
      void visit(const ast::UnaryExpression&) override;
      void visit(const ast::CastExpression&) override;
      void visit(const ast::BinaryExpression&) override;
      void visit(const ast::CallExpression&) override;
      void visit(const ast::ArrayLiteral&) override;
      void visit(const ast::IndexExpression&) override;
      void visit(const ast::StructLiteral&) override;
      void visit(const ast::FieldAccessExpression&) override;
      void visit(const ast::ReturnStatement&) override;
      void visit(const ast::LetStatement&) override;
      void visit(const ast::IfStatement&) override;
      void visit(const ast::WhileStatement&) override;
      void visit(const ast::AssignmentStatement&) override;
      void visit(const ast::ExpressionStatement&) override;

    private:
      PlacesState& state_;
      std::string name_;
      Type type_;
    };
  };

  class TypeChecker::StatementsState final : public TypeCheckerState {
  public:
    explicit StatementsState(TypeChecker& checker) : TypeCheckerState(checker) {}
    bool checkStatements(const std::vector<std::unique_ptr<ast::Statement>>&, Type);
    bool checkStatement(const ast::Statement&, Type);
    void pushScope();
    void popScope();
    bool declareLocal(const std::string&, Type);
    Type lookupLocal(const std::string&, SourceLocation) const;

  private:
    class StatementVisitor final : public internal::StatementOnlyVisitor {
    public:
      using internal::StatementOnlyVisitor::visit;
      StatementVisitor(StatementsState&, Type);
      bool returned() const { return returned_; }
      void visit(const ast::ReturnStatement&) override;
      void visit(const ast::LetStatement&) override;
      void visit(const ast::IfStatement&) override;
      void visit(const ast::WhileStatement&) override;
      void visit(const ast::AssignmentStatement&) override;
      void visit(const ast::ExpressionStatement&) override;

    private:
      StatementsState& state_;
      Type expectedReturnType_;
      bool returned_ = false;
    };
  };

  class TypeChecker::StructsState final : public TypeCheckerState {
  public:
    explicit StructsState(TypeChecker& checker) : TypeCheckerState(checker) {}
    void collectStructDecls(const ast::Module&);
    void collectGenericStructDecl(const ast::StructDecl&, std::size_t);
    void collectConcreteStructDecl(const ast::StructDecl&);
    void validateConcreteStructFieldTypes(const ast::Module&, std::size_t firstStruct = 0);
    bool allowsInternalStructTypes(const ast::StructDecl&) const;
    void checkStructAcyclic(const std::string&, SourceLocation) const;
    const StructInfo& lookupStruct(const std::string&, SourceLocation) const;
    StructInfo resolveStructInfo(const Type&, SourceLocation) const;
    Type checkStructLiteral(const ast::StructLiteral&);
    Type checkGenericStructLiteral(const ast::StructLiteral&, const ast::StructDecl&);
    Type checkConcreteStructLiteral(const ast::StructLiteral&, const StructInfo&,
                                    std::vector<Type>);
    std::vector<Type> inferStructLiteralTypeArgs(const ast::StructLiteral&, const ast::StructDecl&);
    std::unordered_map<std::string, Type> checkStructLiteralFields(const ast::StructLiteral&,
                                                                   const StructInfo&);
    void requireStructLiteralComplete(const ast::StructLiteral&, const StructInfo&,
                                      const std::unordered_map<std::string, Type>&) const;
    std::string structOriginModule(const std::string&) const;
    std::string currentModuleOrigin() const;
    void requireFieldVisible(const std::string&, const StructFieldInfo&, SourceLocation) const;
  };

} // namespace noria
