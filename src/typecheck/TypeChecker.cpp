#include "noria/TypeChecker.hpp"

#include "TypeCheckerState.hpp"

#include "noria/Diagnostic.hpp"
#include "noria/SemanticTables.hpp"

#include <concepts>
#include <memory>
#include <utility>

namespace noria {

  TypeChecker::TypeChecker()
      : relations_(*this), driverState_(std::make_unique<DriverState>(*this)),
        callsState_(std::make_unique<CallsState>(*this)),
        declarationsState_(std::make_unique<DeclarationsState>(*this)),
        expressionsState_(std::make_unique<ExpressionsState>(*this)),
        placesState_(std::make_unique<PlacesState>(*this)),
        statementsState_(std::make_unique<StatementsState>(*this)),
        structsState_(std::make_unique<StructsState>(*this)) {
    static_assert(std::derived_from<DriverState, TypeCheckerState>);
    static_assert(std::derived_from<CallsState, TypeCheckerState>);
    static_assert(std::derived_from<DeclarationsState, TypeCheckerState>);
    static_assert(std::derived_from<ExpressionsState, TypeCheckerState>);
    static_assert(std::derived_from<PlacesState, TypeCheckerState>);
    static_assert(std::derived_from<StatementsState, TypeCheckerState>);
    static_assert(std::derived_from<StructsState, TypeCheckerState>);
  }

  TypeChecker::~TypeChecker() = default;

  TypeChecker::TypeChecker(TypeChecker&& other) noexcept
      : environment_(std::move(other.environment_)), session_(std::move(other.session_)),
        pendingReturnTypeFunctions_(std::move(other.pendingReturnTypeFunctions_)),
        inferringReturnTypes_(other.inferringReturnTypes_), relations_(*this),
        driverState_(std::move(other.driverState_)), callsState_(std::move(other.callsState_)),
        declarationsState_(std::move(other.declarationsState_)),
        expressionsState_(std::move(other.expressionsState_)),
        placesState_(std::move(other.placesState_)),
        statementsState_(std::move(other.statementsState_)),
        structsState_(std::move(other.structsState_)) {
    rebindStates();
  }

  TypeChecker& TypeChecker::operator=(TypeChecker&& other) noexcept {
    if (this != &other) {
      environment_ = std::move(other.environment_);
      session_ = std::move(other.session_);
      pendingReturnTypeFunctions_ = std::move(other.pendingReturnTypeFunctions_);
      inferringReturnTypes_ = other.inferringReturnTypes_;
      driverState_ = std::move(other.driverState_);
      callsState_ = std::move(other.callsState_);
      declarationsState_ = std::move(other.declarationsState_);
      expressionsState_ = std::move(other.expressionsState_);
      placesState_ = std::move(other.placesState_);
      statementsState_ = std::move(other.statementsState_);
      structsState_ = std::move(other.structsState_);
      rebindStates();
    }
    return *this;
  }

  void TypeChecker::rebindStates() noexcept {
    driverState_->rebind(*this);
    callsState_->rebind(*this);
    declarationsState_->rebind(*this);
    expressionsState_->rebind(*this);
    placesState_->rebind(*this);
    statementsState_->rebind(*this);
    structsState_->rebind(*this);
  }

  void TypeChecker::check(ast::Module& module, const SymbolOrigins& origins) {
    driverState_->check(module, origins);
  }

  void TypeChecker::checkSpecializationFrontier(const ast::Module& module, std::size_t firstStruct,
                                                std::size_t firstFunction,
                                                const SymbolOrigins& origins) {
    driverState_->checkSpecializationFrontier(module, firstStruct, firstFunction, origins);
  }

  void TypeChecker::registerFunctionSpecialization(std::string mangledName,
                                                   std::vector<Type> typeArgs) {
    session_.functionSpecializationTypeArgs.emplace(std::move(mangledName), std::move(typeArgs));
  }

  void TypeChecker::registerStructSpecialization(std::string mangledName,
                                                 std::vector<Type> typeArgs) {
    session_.structSpecializationTypeArgs.emplace(std::move(mangledName), std::move(typeArgs));
  }

  Type TypeChecker::canonicalStructType(const Type& type) const {
    return relations_.canonicalStructType(type);
  }

  std::optional<StandardContainer> TypeChecker::standardContainerFor(const Type& type) const {
    const Type canonical = canonicalStructType(type);
    if (canonical.kind != TypeKind::Struct) {
      return std::nullopt;
    }

    const StandardContainerInfo* info =
        standardContainerInfo(structOriginModule(canonical.structName), canonical.structName);
    if (info == nullptr || canonical.typeArgs.size() != info->typeArgumentCount) {
      return std::nullopt;
    }
    return info->kind;
  }

  void TypeChecker::recordImplicitContainerOperation(StandardContainer container,
                                                     ContainerOperation operation,
                                                     const std::vector<Type>& typeArgs,
                                                     SourceLocation location) {
    const std::string name(containerOperationHiddenName(container, operation));
    if (name.empty() || !environment_.genericFunctions.contains(name)) {
      throw CompileError("typecheck: internal error: missing container operation");
    }

    checkSpecializationConstraints(name, typeArgs, location);
    session_.specializationRequests.push_back(
        SpecializationRequest{name, typeArgs, location, session_.currentFunctionName, false});
  }

  void TypeChecker::requireDefaultInitializable(const Type& type, SourceLocation location) {
    const Type canonical = canonicalStructType(type);
    if (const std::optional<StandardContainer> container = standardContainerFor(canonical)) {
      recordImplicitContainerOperation(*container, ContainerOperation::New, canonical.typeArgs,
                                       location);
      return;
    }

    if (canonical.kind != TypeKind::Struct) {
      return;
    }

    const StructInfo info = resolveStructInfo(canonical, location);
    for (const StructFieldInfo& field : info.fields) {
      requireDefaultInitializable(field.type, location);
    }
  }

  const std::vector<SpecializationRequest>& TypeChecker::specializationRequests() const {
    return session_.specializationRequests;
  }
  const std::vector<StructSpecializationRequest>&
  TypeChecker::structSpecializationRequests() const {
    return session_.structSpecializationRequests;
  }
  void TypeChecker::clearSpecializationRequests() { session_.specializationRequests.clear(); }
  void TypeChecker::clearStructSpecializationRequests() {
    session_.structSpecializationRequests.clear();
  }
  std::vector<SpecializationRequest> TypeChecker::takeSpecializationRequests() {
    std::vector<SpecializationRequest> requests;
    requests.swap(session_.specializationRequests);
    return requests;
  }
  std::vector<StructSpecializationRequest> TypeChecker::takeStructSpecializationRequests() const {
    std::vector<StructSpecializationRequest> requests;
    requests.swap(session_.structSpecializationRequests);
    return requests;
  }

  void TypeChecker::requireKnownType(const Type& type, SourceLocation location,
                                     const std::unordered_set<std::string>* params,
                                     bool allowImplTags, bool allowInternalTypes) const {
    relations_.requireKnownType(type, location, params, allowImplTags, allowInternalTypes);
  }
  void TypeChecker::unifyTypes(const Type& expected, const Type& actual,
                               std::unordered_map<std::string, Type>& bindings,
                               SourceLocation location) const {
    relations_.unifyTypes(expected, actual, bindings, location);
  }
  bool TypeChecker::isAssignable(Type expected, Type actual) const {
    return relations_.isAssignable(std::move(expected), std::move(actual));
  }
  void TypeChecker::checkSpecializationConstraints(const std::string& name,
                                                   const std::vector<Type>& args,
                                                   SourceLocation location) const {
    relations_.checkSpecializationConstraints(name, args, location);
  }
  void TypeChecker::recordStructSpecialization(const std::string& name,
                                               const std::vector<Type>& args,
                                               SourceLocation location) const {
    relations_.recordStructSpecialization(name, args, location);
  }

  void TypeChecker::checkFunction(const ast::Function& function) {
    driverState_->checkFunction(function);
  }
  void TypeChecker::inferFunctionReturnTypes(ast::Module& module) {
    driverState_->inferFunctionReturnTypes(module);
  }
  std::optional<Type> TypeChecker::inferFunctionReturnType(const ast::Function& function) {
    return driverState_->inferFunctionReturnType(function);
  }
  void TypeChecker::inferReturnTypesInStatements(
      const std::vector<std::unique_ptr<ast::Statement>>& statements,
      ReturnInferenceResult& result) {
    driverState_->inferReturnTypesInStatements(statements, result);
  }
  void TypeChecker::mergeInferredReturnType(ReturnInferenceResult& result, Type type,
                                            SourceLocation location) {
    driverState_->mergeInferredReturnType(result, std::move(type), location);
  }

  bool TypeChecker::checkStatements(const std::vector<std::unique_ptr<ast::Statement>>& statements,
                                    Type expected) {
    return statementsState_->checkStatements(statements, expected);
  }
  bool TypeChecker::checkStatement(const ast::Statement& statement, Type expected) {
    return statementsState_->checkStatement(statement, expected);
  }
  TypeChecker::PlaceInfo TypeChecker::checkPlace(const ast::Expression& expression) {
    return placesState_->checkPlace(expression);
  }
  Type TypeChecker::checkRvalue(const ast::Expression& expression,
                                std::optional<Type> expectedType) {
    return expressionsState_->checkRvalue(expression, std::move(expectedType));
  }

  void TypeChecker::collectStructDecls(const ast::Module& module) {
    structsState_->collectStructDecls(module);
  }
  void TypeChecker::collectGenericStructDecl(const ast::StructDecl& decl, std::size_t index) {
    structsState_->collectGenericStructDecl(decl, index);
  }
  void TypeChecker::collectConcreteStructDecl(const ast::StructDecl& decl) {
    structsState_->collectConcreteStructDecl(decl);
  }
  void TypeChecker::validateConcreteStructFieldTypes(const ast::Module& module,
                                                     std::size_t firstStruct) {
    structsState_->validateConcreteStructFieldTypes(module, firstStruct);
  }
  bool TypeChecker::allowsInternalStructTypes(const ast::StructDecl& decl) const {
    return structsState_->allowsInternalStructTypes(decl);
  }
  void TypeChecker::checkStructAcyclic(const std::string& name, SourceLocation location) const {
    structsState_->checkStructAcyclic(name, location);
  }
  const TypeChecker::StructInfo& TypeChecker::lookupStruct(const std::string& name,
                                                           SourceLocation location) const {
    return structsState_->lookupStruct(name, location);
  }
  TypeChecker::StructInfo TypeChecker::resolveStructInfo(const Type& type,
                                                         SourceLocation location) const {
    return structsState_->resolveStructInfo(type, location);
  }
  Type TypeChecker::checkStructLiteral(const ast::StructLiteral& literal) {
    return structsState_->checkStructLiteral(literal);
  }
  Type TypeChecker::checkGenericStructLiteral(const ast::StructLiteral& literal,
                                              const ast::StructDecl& decl) {
    return structsState_->checkGenericStructLiteral(literal, decl);
  }
  Type TypeChecker::checkConcreteStructLiteral(const ast::StructLiteral& literal,
                                               const StructInfo& info, std::vector<Type> typeArgs) {
    return structsState_->checkConcreteStructLiteral(literal, info, std::move(typeArgs));
  }
  std::vector<Type>
  TypeChecker::inferStructLiteralTypeArgs(const ast::StructLiteral& literal,
                                          const ast::StructDecl& decl) {
    return structsState_->inferStructLiteralTypeArgs(literal, decl);
  }
  std::unordered_map<std::string, Type>
  TypeChecker::checkStructLiteralFields(const ast::StructLiteral& literal, const StructInfo& info) {
    return structsState_->checkStructLiteralFields(literal, info);
  }
  void TypeChecker::requireStructLiteralComplete(
      const ast::StructLiteral& literal, const StructInfo& info,
      const std::unordered_map<std::string, Type>& present) const {
    structsState_->requireStructLiteralComplete(literal, info, present);
  }

  void TypeChecker::collectFunctionSignatures(const ast::Module& module) {
    declarationsState_->collectFunctionSignatures(module);
  }
  void TypeChecker::collectGenericFunctionSignature(const ast::Function& function,
                                                    std::size_t index) {
    declarationsState_->collectGenericFunctionSignature(function, index);
  }
  void TypeChecker::collectConcreteFunctionSignature(const ast::Function& function) {
    declarationsState_->collectConcreteFunctionSignature(function);
  }
  void TypeChecker::requireDefinableFunctionName(const ast::Function& function) const {
    declarationsState_->requireDefinableFunctionName(function);
  }
  void TypeChecker::validateGenericFunctionFamily(std::string_view name,
                                                  const std::vector<std::size_t>& family) const {
    declarationsState_->validateGenericFunctionFamily(name, family);
  }

  Type TypeChecker::checkBuiltinCall(const ast::CallExpression& call,
                                     const BuiltinSignature& signature) {
    return callsState_->checkBuiltinCall(call, signature);
  }
  void TypeChecker::requireBuiltinCallable(const ast::CallExpression& call,
                                           const BuiltinSignature& signature) const {
    callsState_->requireBuiltinCallable(call, signature);
  }
  Type TypeChecker::checkLenBuiltin(const ast::CallExpression& call) {
    return callsState_->checkLenBuiltin(call);
  }
  Type TypeChecker::checkRtSizeofBuiltin(const ast::CallExpression& call) const {
    return callsState_->checkRtSizeofBuiltin(call);
  }
  Type TypeChecker::checkRtHashBuiltin(const ast::CallExpression& call) {
    return callsState_->checkRtHashBuiltin(call);
  }
  Type TypeChecker::checkRtLoadBuiltin(const ast::CallExpression& call,
                                       const BuiltinSignature& signature) {
    return callsState_->checkRtLoadBuiltin(call, signature);
  }
  Type TypeChecker::checkRtStoreBuiltin(const ast::CallExpression& call,
                                        const BuiltinSignature& signature) {
    return callsState_->checkRtStoreBuiltin(call, signature);
  }
  Type TypeChecker::checkAllArgumentsBuiltin(const ast::CallExpression& call,
                                             const BuiltinSignature& signature) {
    return callsState_->checkAllArgumentsBuiltin(call, signature);
  }
  Type TypeChecker::checkDeclaredBuiltinArguments(const ast::CallExpression& call,
                                                  const BuiltinSignature& signature) {
    return callsState_->checkDeclaredBuiltinArguments(call, signature);
  }
  Type TypeChecker::checkGenericFunctionCall(const ast::CallExpression& call,
                                             const std::vector<std::size_t>& family,
                                             const std::optional<Type>& expected) {
    return callsState_->checkGenericFunctionCall(call, family, expected);
  }
  Type TypeChecker::checkConcreteFunctionCall(const ast::CallExpression& call,
                                              const FunctionSignature& signature) {
    return callsState_->checkConcreteFunctionCall(call, signature);
  }
  std::vector<Type> TypeChecker::inferGenericCallTypeArgs(
      const ast::CallExpression& call, const ast::Function& function, bool allowInternal,
      const std::optional<Type>& expected, std::unordered_map<std::string, Type>& bindings) {
    return callsState_->inferGenericCallTypeArgs(call, function, allowInternal, expected, bindings);
  }
  Type TypeChecker::resolveWitnessType(SourceLocation location) const {
    return callsState_->resolveWitnessType(location);
  }
  bool TypeChecker::isEnclosingFunctionSpecialized() const {
    return callsState_->isEnclosingFunctionSpecialized();
  }
  const std::vector<Type>* TypeChecker::enclosingFunctionSpecializationTypeArgs() const {
    return callsState_->enclosingFunctionSpecializationTypeArgs();
  }
  void TypeChecker::seedMatchingTypeParamsFromCaller(
      std::unordered_map<std::string, Type>& bindings,
      const std::vector<ast::TypeParameter>& params) const {
    callsState_->seedMatchingTypeParamsFromCaller(bindings, params);
  }
  void TypeChecker::seedUnboundTypeParamsFromCaller(
      std::unordered_map<std::string, Type>& bindings,
      const std::vector<ast::TypeParameter>& params) const {
    callsState_->seedUnboundTypeParamsFromCaller(bindings, params);
  }
  void TypeChecker::seedUnboundTypeParamsFromExpectedType(
      std::unordered_map<std::string, Type>& bindings, const Type& pattern,
      const std::optional<Type>& expected, SourceLocation location) const {
    callsState_->seedUnboundTypeParamsFromExpectedType(bindings, pattern, expected, location);
  }

  Type TypeChecker::checkBinaryExpression(const ast::BinaryExpression& binary, const Type& left,
                                          const Type& right) const {
    return expressionsState_->checkBinaryExpression(binary, left, right);
  }
  void TypeChecker::rejectStaticallyInvalidIntegerOperation(const ast::BinaryExpression& binary,
                                                            const Type& result) const {
    expressionsState_->rejectStaticallyInvalidIntegerOperation(binary, result);
  }
  Type TypeChecker::checkLogicalBinaryExpression(const ast::BinaryExpression& binary,
                                                 const Type& left, const Type& right) const {
    return expressionsState_->checkLogicalBinaryExpression(binary, left, right);
  }
  Type TypeChecker::checkAdditiveBinaryExpression(const ast::BinaryExpression& binary,
                                                  const Type& left, const Type& right) const {
    return expressionsState_->checkAdditiveBinaryExpression(binary, left, right);
  }
  std::optional<Type> TypeChecker::sequenceElementType(const Type& type) const {
    return expressionsState_->sequenceElementType(type);
  }
  bool TypeChecker::supportsCollectionAddition(const Type& type) const {
    return expressionsState_->supportsCollectionAddition(type);
  }
  Type TypeChecker::checkIntegerBinaryExpression(const ast::BinaryExpression& binary,
                                                 const Type& left, const Type& right) const {
    return expressionsState_->checkIntegerBinaryExpression(binary, left, right);
  }
  Type TypeChecker::checkOrderedComparisonExpression(const ast::BinaryExpression& binary,
                                                     const Type& left, const Type& right) const {
    return expressionsState_->checkOrderedComparisonExpression(binary, left, right);
  }
  Type TypeChecker::checkEqualityExpression(const ast::BinaryExpression& binary, const Type& left,
                                            const Type& right) const {
    return expressionsState_->checkEqualityExpression(binary, left, right);
  }
  Type TypeChecker::checkUnaryExpression(const ast::UnaryExpression& unary,
                                         const Type& operand) const {
    return expressionsState_->checkUnaryExpression(unary, operand);
  }
  Type TypeChecker::checkNumericUnaryExpression(const ast::UnaryExpression& unary,
                                                const Type& operand) const {
    return expressionsState_->checkNumericUnaryExpression(unary, operand);
  }
  Type TypeChecker::checkBooleanUnaryExpression(const ast::UnaryExpression& unary,
                                                const Type& operand) const {
    return expressionsState_->checkBooleanUnaryExpression(unary, operand);
  }
  Type TypeChecker::checkIntegerUnaryExpression(const ast::UnaryExpression& unary,
                                                const Type& operand) const {
    return expressionsState_->checkIntegerUnaryExpression(unary, operand);
  }

  void TypeChecker::pushScope() { statementsState_->pushScope(); }
  void TypeChecker::popScope() { statementsState_->popScope(); }
  bool TypeChecker::declareLocal(const std::string& name, Type type) {
    return statementsState_->declareLocal(name, std::move(type));
  }
  Type TypeChecker::lookupLocal(const std::string& name, SourceLocation location) const {
    return statementsState_->lookupLocal(name, location);
  }
  bool TypeChecker::isStdlibOrigin(const std::string& modulePath) const {
    return declarationsState_->isStdlibOrigin(modulePath);
  }
  bool TypeChecker::isInternalModuleOrigin(const std::string& modulePath) const {
    return declarationsState_->isInternalModuleOrigin(modulePath);
  }
  bool TypeChecker::isStdlibContext() const { return declarationsState_->isStdlibContext(); }
  void TypeChecker::requireFunctionCallable(const std::string& name,
                                            SourceLocation location) const {
    declarationsState_->requireFunctionCallable(name, location);
  }
  std::string TypeChecker::structOriginModule(const std::string& structName) const {
    return structsState_->structOriginModule(structName);
  }
  std::string TypeChecker::currentModuleOrigin() const {
    return structsState_->currentModuleOrigin();
  }
  void TypeChecker::requireFieldVisible(const std::string& structName, const StructFieldInfo& field,
                                        SourceLocation location) const {
    structsState_->requireFieldVisible(structName, field, location);
  }
  const ast::Function& TypeChecker::genericFunctionAt(std::size_t index) const {
    return declarationsState_->genericFunctionAt(index);
  }
  const ast::StructDecl& TypeChecker::genericStructAt(std::size_t index) const {
    return declarationsState_->genericStructAt(index);
  }
  bool TypeChecker::allowsInternalFunctionTypes(const ast::Function& function) const {
    return declarationsState_->allowsInternalFunctionTypes(function);
  }

} // namespace noria
