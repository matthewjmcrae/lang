#pragma once

#include "noria/Ast.hpp"
#include "noria/AstVisitor.hpp"
#include "noria/Builtins.hpp"
#include "noria/HashTable.hpp"
#include "noria/ModuleResolver.hpp"
#include "noria/Monomorphize.hpp"
#include "noria/SemanticTables.hpp"
#include "noria/TypeChecker.hpp"

#include "../internal/AstVisitorAdapters.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace noria::typecheck_detail {

  using Scope = std::unordered_map<std::string, Type>;

  struct PlaceInfo {
    std::string name;
    Type type;
  };

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

  struct TypeEnvironment {
    const ast::Module* activeModule = nullptr;
    SymbolOrigins symbolOrigins;
    std::unordered_map<std::string, FunctionSignature> functions;
    std::unordered_map<std::string, std::vector<std::size_t>> genericFunctions;
    HashTable<std::string, std::size_t> genericStructs;
    std::unordered_map<std::string, StructInfo> structs;
  };

  class ScopeStack {
  public:
    class Frame {
    public:
      explicit Frame(ScopeStack& scopes) : scopes_(scopes) { scopes_.push(); }
      ~Frame() { scopes_.pop(); }
      Frame(const Frame&) = delete;
      Frame& operator=(const Frame&) = delete;

    private:
      ScopeStack& scopes_;
    };

    void clear() { scopes_.clear(); }
    void push() { scopes_.emplace_back(); }
    void pop() { scopes_.pop_back(); }
    Frame frame() { return Frame(*this); }
    bool declare(const std::string&, Type);
    Type lookup(const std::string&, SourceLocation) const;

  private:
    std::vector<Scope> scopes_;
  };

  class SpecializationRegistry {
  public:
    void registerFunction(std::string, std::vector<Type>);
    void registerStruct(std::string, std::vector<Type>);
    const std::vector<SpecializationRequest>& functionRequests() const;
    const std::vector<StructSpecializationRequest>& structRequests() const;
    void clearRequests();
    void clearFunctionRequests();
    void clearStructRequests();
    std::vector<SpecializationRequest> takeFunctionRequests();
    std::vector<StructSpecializationRequest> takeStructRequests() const;
    void recordFunctionRequest(SpecializationRequest);
    void recordStructRequest(StructSpecializationRequest);
    const std::unordered_map<std::string, std::vector<Type>>& functionTypeArgs() const;
    const std::unordered_map<std::string, std::vector<Type>>& structTypeArgs() const;

  private:
    std::vector<SpecializationRequest> functionRequests_;
    mutable std::vector<StructSpecializationRequest> structRequests_;
    std::unordered_map<std::string, std::vector<Type>> functionTypeArgs_;
    std::unordered_map<std::string, std::vector<Type>> structTypeArgs_;
  };

  struct TypeCheckSession {
    std::string currentFunctionName;
  };

  struct TypeCheckContext {
    TypeEnvironment environment;
    TypeCheckSession session;
    ScopeStack scopes;
    SpecializationRegistry specializations;
  };

  class TypeCheckComponent {
  protected:
    explicit TypeCheckComponent(TypeCheckContext& context) : context_(context) {}
    TypeEnvironment& environment() { return context_.environment; }
    const TypeEnvironment& environment() const { return context_.environment; }
    TypeCheckSession& session() { return context_.session; }
    const TypeCheckSession& session() const { return context_.session; }
    ScopeStack& scopes() { return context_.scopes; }
    const ScopeStack& scopes() const { return context_.scopes; }
    SpecializationRegistry& specializations() { return context_.specializations; }
    SpecializationRegistry& specializations() const { return context_.specializations; }

    TypeCheckContext& context_;
  };

  class TypeRelations final : private TypeCheckComponent {
  public:
    explicit TypeRelations(TypeCheckContext& context) : TypeCheckComponent(context) {}
    void requireKnownType(const Type&, SourceLocation, const std::unordered_set<std::string>* = nullptr,
                          bool allowImplTags = false, bool allowInternalTypes = false) const;
    void unifyTypes(const Type&, const Type&, std::unordered_map<std::string, Type>&,
                    SourceLocation) const;
    bool isAssignable(Type, Type) const;
    void checkSpecializationConstraints(const std::string&, const std::vector<Type>&,
                                        SourceLocation) const;
    void recordStructSpecialization(const std::string&, const std::vector<Type>&,
                                    SourceLocation) const;
    Type canonicalStructType(const Type&) const;
    std::optional<StandardContainer> standardContainerFor(const Type&) const;
    void recordImplicitContainerOperation(StandardContainer, ContainerOperation,
                                          const std::vector<Type>&, SourceLocation);
    void requireContainerOwnershipOps(const Type&, SourceLocation);

  private:
    void requireRawPtrUsable(const Type&, SourceLocation, bool) const;
    void requireImplTagUsable(const Type&, SourceLocation, bool) const;
    void requireTypeParamKnown(const Type&, SourceLocation,
                               const std::unordered_set<std::string>*) const;
    void requireArrayTypeKnown(const Type&, SourceLocation,
                               const std::unordered_set<std::string>*, bool, bool) const;
    void requireStructTypeKnown(const Type&, SourceLocation,
                                const std::unordered_set<std::string>*, bool) const;
    void bindTypeParam(const Type&, const Type&, std::unordered_map<std::string, Type>&,
                       SourceLocation) const;
    void unifyArrayTypes(const Type&, const Type&, std::unordered_map<std::string, Type>&,
                         SourceLocation) const;
    void unifyImplTagTypes(const Type&, const Type&, SourceLocation) const;
    void unifyStructTypes(const Type&, const Type&, std::unordered_map<std::string, Type>&,
                          SourceLocation) const;
  };

  class DeclarationChecker final : private TypeCheckComponent {
  public:
    DeclarationChecker(TypeCheckContext&, TypeRelations&);
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

  private:
    TypeRelations& relations_;
  };

  class ExpressionChecker;

  class CallChecker final : private TypeCheckComponent {
  public:
    CallChecker(TypeCheckContext&, TypeRelations&, DeclarationChecker&);
    Type checkBuiltinCall(ExpressionChecker&, const ast::CallExpression&, const BuiltinSignature&);
    Type checkGenericFunctionCall(ExpressionChecker&, const ast::CallExpression&,
                                  const std::vector<std::size_t>&, const std::optional<Type>&);
    Type checkConcreteFunctionCall(ExpressionChecker&, const ast::CallExpression&,
                                   const FunctionSignature&);

  private:
    void requireBuiltinCallable(const ast::CallExpression&, const BuiltinSignature&) const;
    Type checkLenBuiltin(ExpressionChecker&, const ast::CallExpression&);
    Type checkRtSizeofBuiltin(const ast::CallExpression&) const;
    Type checkRtHashBuiltin(ExpressionChecker&, const ast::CallExpression&);
    Type checkRtLoadBuiltin(ExpressionChecker&, const ast::CallExpression&, const BuiltinSignature&);
    Type checkRtStoreBuiltin(ExpressionChecker&, const ast::CallExpression&, const BuiltinSignature&);
    Type checkRtDropBuiltin(ExpressionChecker&, const ast::CallExpression&, const BuiltinSignature&);
    Type checkAllArgumentsBuiltin(ExpressionChecker&, const ast::CallExpression&, const BuiltinSignature&);
    Type checkDeclaredBuiltinArguments(ExpressionChecker&, const ast::CallExpression&,
                                       const BuiltinSignature&);
    std::vector<Type> inferGenericCallTypeArgs(ExpressionChecker&, const ast::CallExpression&,
                                               const ast::Function&, bool,
                                               const std::optional<Type>&,
                                               std::unordered_map<std::string, Type>&);
    Type resolveWitnessType(SourceLocation) const;
    const std::vector<Type>* enclosingFunctionSpecializationTypeArgs() const;
    void seedMatchingTypeParamsFromCaller(std::unordered_map<std::string, Type>&,
                                          const std::vector<ast::TypeParameter>&) const;
    void seedUnboundTypeParamsFromCaller(std::unordered_map<std::string, Type>&,
                                         const std::vector<ast::TypeParameter>&) const;
    void seedUnboundTypeParamsFromExpectedType(std::unordered_map<std::string, Type>&, const Type&,
                                               const std::optional<Type>&, SourceLocation) const;

    TypeRelations& relations_;
    DeclarationChecker& declarations_;
  };

  class StructChecker;
  class TypeCheckDriver;

  class ExpressionChecker final : private TypeCheckComponent {
  public:
    ExpressionChecker(TypeCheckContext&, TypeRelations&, DeclarationChecker&, CallChecker&, StructChecker&);
    void setDriver(const TypeCheckDriver& driver) { driver_ = &driver; }
    Type checkRvalue(const ast::Expression&, std::optional<Type> expectedType = std::nullopt);

  private:
    class ExpressionVisitor final : public internal::ExpressionOnlyVisitor {
    public:
      using internal::ExpressionOnlyVisitor::visit;
      ExpressionVisitor(ExpressionChecker&, std::optional<Type>);
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
      ExpressionChecker& state_;
      std::optional<Type> expectedType_;
      Type result_;
    };

    Type checkBinaryExpression(const ast::BinaryExpression&, const Type&, const Type&) const;
    void rejectStaticallyInvalidIntegerOperation(const ast::BinaryExpression&, const Type&) const;
    Type checkLogicalBinaryExpression(const ast::BinaryExpression&, const Type&, const Type&) const;
    Type checkAdditiveBinaryExpression(const ast::BinaryExpression&, const Type&, const Type&) const;
    std::optional<Type> sequenceElementType(const Type&) const;
    bool supportsCollectionAddition(const Type&) const;
    Type checkIntegerBinaryExpression(const ast::BinaryExpression&, const Type&, const Type&) const;
    Type checkOrderedComparisonExpression(const ast::BinaryExpression&, const Type&, const Type&) const;
    Type checkEqualityExpression(const ast::BinaryExpression&, const Type&, const Type&) const;
    Type checkUnaryExpression(const ast::UnaryExpression&, const Type&) const;
    Type checkNumericUnaryExpression(const ast::UnaryExpression&, const Type&) const;
    Type checkBooleanUnaryExpression(const ast::UnaryExpression&, const Type&) const;
    Type checkIntegerUnaryExpression(const ast::UnaryExpression&, const Type&) const;

    Type lookupLocal(const std::string& name, SourceLocation location) const {
      return scopes().lookup(name, location);
    }
    bool isStdlibContext() const { return declarations_.isStdlibContext(); }
    void requireKnownType(const Type& type, SourceLocation location,
                          const std::unordered_set<std::string>* params = nullptr,
                          bool allowImplTags = false, bool allowInternalTypes = false) const {
      relations_.requireKnownType(type, location, params, allowImplTags, allowInternalTypes);
    }
    Type canonicalStructType(const Type& type) const { return relations_.canonicalStructType(type); }
    std::optional<StandardContainer> standardContainerFor(const Type& type) const {
      return relations_.standardContainerFor(type);
    }
    bool isAssignable(Type expected, Type actual) const {
      return relations_.isAssignable(std::move(expected), std::move(actual));
    }
    void recordImplicitContainerOperation(StandardContainer container, ContainerOperation operation,
                                          const std::vector<Type>& typeArgs, SourceLocation location) {
      relations_.recordImplicitContainerOperation(container, operation, typeArgs, location);
    }
    void requireDefaultInitializable(const Type&, SourceLocation);
    StructInfo resolveStructInfo(const Type&, SourceLocation) const;
    void requireFieldVisible(const std::string&, const StructFieldInfo&, SourceLocation) const;

    TypeRelations& relations_;
    DeclarationChecker& declarations_;
    CallChecker& calls_;
    StructChecker& structs_;
    const TypeCheckDriver* driver_ = nullptr;
  };

  class StructChecker final : private TypeCheckComponent {
  public:
    StructChecker(TypeCheckContext&, TypeRelations&, DeclarationChecker&);
    void collectStructDecls(const ast::Module&);
    void collectGenericStructDecl(const ast::StructDecl&, std::size_t);
    void collectConcreteStructDecl(const ast::StructDecl&);
    void validateConcreteStructFieldTypes(const ast::Module&, std::size_t firstStruct = 0);
    bool allowsInternalStructTypes(const ast::StructDecl&) const;
    void checkStructAcyclic(const std::string&, SourceLocation) const;
    const StructInfo& lookupStruct(const std::string&, SourceLocation) const;
    StructInfo resolveStructInfo(const Type&, SourceLocation) const;
    Type checkStructLiteral(ExpressionChecker&, const ast::StructLiteral&);
    void requireDefaultInitializable(const Type&, SourceLocation);
    void requireFieldVisible(const std::string&, const StructFieldInfo&, SourceLocation) const;

  private:
    Type checkGenericStructLiteral(ExpressionChecker&, const ast::StructLiteral&, const ast::StructDecl&);
    Type checkConcreteStructLiteral(ExpressionChecker&, const ast::StructLiteral&, const StructInfo&,
                                    std::vector<Type>);
    std::vector<Type> inferStructLiteralTypeArgs(ExpressionChecker&, const ast::StructLiteral&,
                                                 const ast::StructDecl&);
    std::unordered_map<std::string, Type> checkStructLiteralFields(ExpressionChecker&,
                                                                    const ast::StructLiteral&,
                                                                    const StructInfo&);
    void requireStructLiteralComplete(const ast::StructLiteral&, const StructInfo&,
                                      const std::unordered_map<std::string, Type>&) const;
    std::string structOriginModule(const std::string&) const;
    std::string currentModuleOrigin() const;

    TypeRelations& relations_;
    DeclarationChecker& declarations_;
  };

  inline void ExpressionChecker::requireDefaultInitializable(const Type& type,
                                                             SourceLocation location) {
    structs_.requireDefaultInitializable(type, location);
  }

  inline StructInfo ExpressionChecker::resolveStructInfo(const Type& type,
                                                         SourceLocation location) const {
    return structs_.resolveStructInfo(type, location);
  }

  inline void ExpressionChecker::requireFieldVisible(const std::string& structName,
                                                      const StructFieldInfo& field,
                                                      SourceLocation location) const {
    structs_.requireFieldVisible(structName, field, location);
  }

  class PlaceChecker final : private TypeCheckComponent {
  public:
    PlaceChecker(TypeCheckContext&, TypeRelations&, ExpressionChecker&, StructChecker&);
    PlaceInfo checkPlace(const ast::Expression&);

  private:
    class PlaceVisitor final : public ast::AstVisitor {
    public:
      explicit PlaceVisitor(PlaceChecker&);
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
      PlaceChecker& state_;
      std::string name_;
      Type type_;
    };

    TypeRelations& relations_;
    ExpressionChecker& expressions_;
    StructChecker& structs_;
    Type lookupLocal(const std::string& name, SourceLocation location) const {
      return scopes().lookup(name, location);
    }
    Type checkRvalue(const ast::Expression& expression,
                     std::optional<Type> expected = std::nullopt) {
      return expressions_.checkRvalue(expression, std::move(expected));
    }
    Type canonicalStructType(const Type& type) const { return relations_.canonicalStructType(type); }
    std::optional<StandardContainer> standardContainerFor(const Type& type) const {
      return relations_.standardContainerFor(type);
    }
    bool isAssignable(Type expected, Type actual) const {
      return relations_.isAssignable(std::move(expected), std::move(actual));
    }
    void recordImplicitContainerOperation(StandardContainer container, ContainerOperation operation,
                                          const std::vector<Type>& typeArgs, SourceLocation location) {
      relations_.recordImplicitContainerOperation(container, operation, typeArgs, location);
    }
    StructInfo resolveStructInfo(const Type& type, SourceLocation location) const {
      return structs_.resolveStructInfo(type, location);
    }
    void requireFieldVisible(const std::string& structName, const StructFieldInfo& field,
                             SourceLocation location) const {
      structs_.requireFieldVisible(structName, field, location);
    }
  };

  class StatementChecker final : private TypeCheckComponent {
  public:
    StatementChecker(TypeCheckContext&, TypeRelations&, DeclarationChecker&, ExpressionChecker&,
                     PlaceChecker&, StructChecker&);
    bool checkStatements(const std::vector<std::unique_ptr<ast::Statement>>&, Type);
    bool checkStatement(const ast::Statement&, Type);

  private:
    class StatementVisitor final : public internal::StatementOnlyVisitor {
    public:
      using internal::StatementOnlyVisitor::visit;
      StatementVisitor(StatementChecker&, Type);
      bool returned() const { return returned_; }
      void visit(const ast::ReturnStatement&) override;
      void visit(const ast::LetStatement&) override;
      void visit(const ast::IfStatement&) override;
      void visit(const ast::WhileStatement&) override;
      void visit(const ast::AssignmentStatement&) override;
      void visit(const ast::ExpressionStatement&) override;

    private:
      StatementChecker& state_;
      Type expectedReturnType_;
      bool returned_ = false;
    };

    TypeRelations& relations_;
    DeclarationChecker& declarations_;
    ExpressionChecker& expressions_;
    PlaceChecker& places_;
    StructChecker& structs_;
    bool isStdlibContext() const { return declarations_.isStdlibContext(); }
    void requireKnownType(const Type& type, SourceLocation location,
                          const std::unordered_set<std::string>* params = nullptr,
                          bool allowImplTags = false, bool allowInternalTypes = false) const {
      relations_.requireKnownType(type, location, params, allowImplTags, allowInternalTypes);
    }
    Type checkRvalue(const ast::Expression& expression,
                     std::optional<Type> expected = std::nullopt) {
      return expressions_.checkRvalue(expression, std::move(expected));
    }
    bool isAssignable(Type expected, Type actual) const {
      return relations_.isAssignable(std::move(expected), std::move(actual));
    }
    void requireDefaultInitializable(const Type& type, SourceLocation location) {
      structs_.requireDefaultInitializable(type, location);
    }
    bool declareLocal(const std::string& name, Type type) { return scopes().declare(name, std::move(type)); }
    ScopeStack::Frame scopeFrame() { return scopes().frame(); }
    void requireContainerOwnershipOps(const Type& type, SourceLocation location) {
      relations_.requireContainerOwnershipOps(type, location);
    }
    PlaceInfo checkPlace(const ast::Expression& expression) { return places_.checkPlace(expression); }
  };

  struct ReturnInferenceResult {
    std::optional<Type> type;
    bool sawPendingCall = false;
  };
  struct ReturnInferencePending {};

  class TypeCheckDriver final : private TypeCheckComponent {
  public:
    TypeCheckDriver(TypeCheckContext&, TypeRelations&, DeclarationChecker&, ExpressionChecker&,
                    StatementChecker&, StructChecker&);
    void check(ast::Module&, const SymbolOrigins&);
    void checkSpecializationFrontier(const ast::Module&, std::size_t, std::size_t,
                                     const SymbolOrigins&);
    bool inferringReturnTypes() const { return inferringReturnTypes_; }
    const std::unordered_set<std::string>& pendingReturnTypeFunctions() const {
      return pendingReturnTypeFunctions_;
    }

  private:
    void checkFunction(const ast::Function&);
    void inferFunctionReturnTypes(ast::Module&);
    std::optional<Type> inferFunctionReturnType(const ast::Function&);
    void inferReturnTypesInStatements(const std::vector<std::unique_ptr<ast::Statement>>&, ReturnInferenceResult&);
    void mergeInferredReturnType(ReturnInferenceResult&, Type, SourceLocation);
    void resetForCheck(ast::Module&, const SymbolOrigins&);

    TypeRelations& relations_;
    DeclarationChecker& declarations_;
    ExpressionChecker& expressions_;
    StatementChecker& statements_;
    StructChecker& structs_;
    std::unordered_set<std::string> pendingReturnTypeFunctions_;
    bool inferringReturnTypes_ = false;
  };

} // namespace noria::typecheck_detail

namespace noria {

  class TypeChecker::Impl {
  public:
    typecheck_detail::TypeCheckContext context;
    typecheck_detail::TypeRelations relations{context};
    typecheck_detail::DeclarationChecker declarations{context, relations};
    typecheck_detail::CallChecker calls{context, relations, declarations};
    typecheck_detail::StructChecker structs{context, relations, declarations};
    typecheck_detail::ExpressionChecker expressions{context, relations, declarations, calls, structs};
    typecheck_detail::PlaceChecker places{context, relations, expressions, structs};
    typecheck_detail::StatementChecker statements{context, relations, declarations, expressions, places,
                                                   structs};
    typecheck_detail::TypeCheckDriver driver{context, relations, declarations, expressions, statements,
                                              structs};

    Impl() { expressions.setDriver(driver); }
  };

} // namespace noria
