#pragma once

#include "noria/Ast.hpp"
#include "noria/Builtins.hpp"
#include "noria/HashTable.hpp"
#include "noria/ModuleResolver.hpp"
#include "noria/SemanticTables.hpp"
#include "noria/Monomorphize.hpp"
#include "noria/Types.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace noria {

  struct FunctionSignature {
    Type returnType;
    std::vector<Type> parameterTypes;
  };

  class TypeChecker {
  public:
    TypeChecker();
    ~TypeChecker();
    TypeChecker(const TypeChecker&) = delete;
    TypeChecker& operator=(const TypeChecker&) = delete;
    TypeChecker(TypeChecker&& other) noexcept;
    TypeChecker& operator=(TypeChecker&& other) noexcept;

    void check(ast::Module& module, const SymbolOrigins& symbolOrigins = {});
    void checkSpecializationFrontier(const ast::Module& module, std::size_t firstNewStruct,
                                     std::size_t firstNewFunction,
                                     const SymbolOrigins& symbolOrigins = {});
    void registerFunctionSpecialization(std::string mangledName, std::vector<Type> typeArgs);
    void registerStructSpecialization(std::string mangledName, std::vector<Type> typeArgs);
    const std::vector<SpecializationRequest>& specializationRequests() const;
    const std::vector<StructSpecializationRequest>& structSpecializationRequests() const;
    void clearSpecializationRequests();
    void clearStructSpecializationRequests();
    std::vector<SpecializationRequest> takeSpecializationRequests();
    std::vector<StructSpecializationRequest> takeStructSpecializationRequests() const;

  private:
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
    struct TypeCheckSession {
      std::vector<SpecializationRequest> specializationRequests;
      mutable std::vector<StructSpecializationRequest> structSpecializationRequests;
      std::string currentFunctionName;
      std::unordered_map<std::string, std::vector<Type>> functionSpecializationTypeArgs;
      std::unordered_map<std::string, std::vector<Type>> structSpecializationTypeArgs;
      std::vector<Scope> scopes;
    };
    struct ReturnInferenceResult {
      std::optional<Type> type;
      bool sawPendingCall = false;
    };
    struct ReturnInferencePending {};

    class TypeRelations {
    public:
      explicit TypeRelations(TypeChecker& checker) : checker_(checker) {}
      void requireKnownType(const Type&, SourceLocation, const std::unordered_set<std::string>*,
                            bool, bool) const;
      void requireRawPtrUsable(const Type&, SourceLocation, bool) const;
      void requireImplTagUsable(const Type&, SourceLocation, bool) const;
      void requireTypeParamKnown(const Type&, SourceLocation,
                                 const std::unordered_set<std::string>*) const;
      void requireArrayTypeKnown(const Type&, SourceLocation,
                                 const std::unordered_set<std::string>*, bool, bool) const;
      void requireStructTypeKnown(const Type&, SourceLocation,
                                  const std::unordered_set<std::string>*, bool) const;
      void unifyTypes(const Type&, const Type&, std::unordered_map<std::string, Type>&,
                      SourceLocation) const;
      void bindTypeParam(const Type&, const Type&, std::unordered_map<std::string, Type>&,
                         SourceLocation) const;
      void unifyArrayTypes(const Type&, const Type&, std::unordered_map<std::string, Type>&,
                           SourceLocation) const;
      void unifyImplTagTypes(const Type&, const Type&, SourceLocation) const;
      void unifyStructTypes(const Type&, const Type&, std::unordered_map<std::string, Type>&,
                            SourceLocation) const;
      void checkSpecializationConstraints(const std::string&, const std::vector<Type>&,
                                          SourceLocation) const;
      void recordStructSpecialization(const std::string&, const std::vector<Type>&,
                                      SourceLocation) const;
      bool isAssignable(Type, Type) const;
      Type canonicalStructType(const Type&) const;

    private:
      TypeChecker& checker_;
    };
    class TypeCheckerState;
    class DriverState;
    class CallsState;
    class DeclarationsState;
    class ExpressionsState;
    class PlacesState;
    class StatementsState;
    class StructsState;

    void rebindStates() noexcept;
    void checkFunction(const ast::Function&);
    void inferFunctionReturnTypes(ast::Module&);
    std::optional<Type> inferFunctionReturnType(const ast::Function&);
    void inferReturnTypesInStatements(const std::vector<std::unique_ptr<ast::Statement>>&,
                                      ReturnInferenceResult&);
    void mergeInferredReturnType(ReturnInferenceResult&, Type, SourceLocation);
    bool checkStatements(const std::vector<std::unique_ptr<ast::Statement>>&, Type);
    bool checkStatement(const ast::Statement&, Type);
    PlaceInfo checkPlace(const ast::Expression&);
    Type checkRvalue(const ast::Expression&, std::optional<Type> expectedType = std::nullopt);

    void collectStructDecls(const ast::Module&);
    void collectGenericStructDecl(const ast::StructDecl&, std::size_t);
    void collectConcreteStructDecl(const ast::StructDecl&);
    void validateConcreteStructFieldTypes(const ast::Module&, std::size_t firstStruct = 0);
    bool allowsInternalStructTypes(const ast::StructDecl&) const;
    bool allowsInternalFunctionTypes(const ast::Function&) const;
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

    void collectFunctionSignatures(const ast::Module&);
    void collectGenericFunctionSignature(const ast::Function&, std::size_t);
    void collectConcreteFunctionSignature(const ast::Function&);
    void requireDefinableFunctionName(const ast::Function&) const;
    void validateGenericFunctionFamily(std::string_view, const std::vector<std::size_t>&) const;
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

    void requireKnownType(const Type&, SourceLocation,
                          const std::unordered_set<std::string>* = nullptr,
                          bool allowImplTags = false, bool allowInternalTypes = false) const;
    void unifyTypes(const Type&, const Type&, std::unordered_map<std::string, Type>&,
                    SourceLocation) const;
    bool isAssignable(Type, Type) const;
    void checkSpecializationConstraints(const std::string&, const std::vector<Type>&,
                                        SourceLocation) const;
    void recordStructSpecialization(const std::string&, const std::vector<Type>&,
                                    SourceLocation) const;
    std::optional<StandardContainer> standardContainerFor(const Type&) const;
    Type canonicalStructType(const Type&) const;
    void recordImplicitContainerOperation(StandardContainer, ContainerOperation,
                                          const std::vector<Type>&, SourceLocation);
    void requireDefaultInitializable(const Type&, SourceLocation);
    void requireContainerOwnershipOps(const Type&, SourceLocation);

    void pushScope();
    void popScope();
    bool declareLocal(const std::string&, Type);
    Type lookupLocal(const std::string&, SourceLocation) const;
    bool isStdlibOrigin(const std::string&) const;
    bool isInternalModuleOrigin(const std::string&) const;
    bool isStdlibContext() const;
    void requireFunctionCallable(const std::string&, SourceLocation) const;
    std::string structOriginModule(const std::string&) const;
    std::string currentModuleOrigin() const;
    void requireFieldVisible(const std::string&, const StructFieldInfo&, SourceLocation) const;
    const ast::Function& genericFunctionAt(std::size_t) const;
    const ast::StructDecl& genericStructAt(std::size_t) const;

    TypeEnvironment environment_;
    TypeCheckSession session_;
    std::unordered_set<std::string> pendingReturnTypeFunctions_;
    bool inferringReturnTypes_ = false;
    TypeRelations relations_;
    std::unique_ptr<DriverState> driverState_;
    std::unique_ptr<CallsState> callsState_;
    std::unique_ptr<DeclarationsState> declarationsState_;
    std::unique_ptr<ExpressionsState> expressionsState_;
    std::unique_ptr<PlacesState> placesState_;
    std::unique_ptr<StatementsState> statementsState_;
    std::unique_ptr<StructsState> structsState_;
  };

} // namespace noria
