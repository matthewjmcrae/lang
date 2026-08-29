#include "noria/Codegen.hpp"

#include "CodegenState.hpp"

#include "noria/Diagnostic.hpp"
#include "noria/Monomorphize.hpp"
#include "noria/SemanticTables.hpp"

#include <concepts>
#include <utility>

namespace noria {

  LLVMGenerator::Value LLVMGenerator::CodegenState::emitStandardContainerCall(
      StandardContainer container, ContainerOperation operation, const std::vector<Type>& typeArgs,
      const std::vector<Value>& arguments, IREmitter& emitter,
      FunctionCodegenContext& context) const {
    const std::string callee =
        mangleSpecialization(containerOperationHiddenName(container, operation), typeArgs);
    const auto function = context.functions.find(callee);
    if (function == context.functions.end()) {
      throw CompileError("codegen: missing container operation");
    }

    const bool returnsVoid = function->second.returnType == Type::voidType();
    const std::string result = returnsVoid ? "" : emitter.freshTemp();
    std::string call = returnsVoid ? "call void @" + callee + "("
                                   : result + " = call " + LLVMType(function->second.returnType) +
                                         " @" + callee + "(";
    for (std::size_t argument{}; argument < arguments.size(); ++argument) {
      if (argument != 0) {
        call += ", ";
      }
      call += LLVMType(arguments[argument].type) + " " + arguments[argument].text;
    }
    call += ")";
    emitter.line(call);
    return Value{result, function->second.returnType};
  }

  std::vector<Type> LLVMGenerator::CodegenState::specializedStructTypeArgs(
      const Type& type, const FunctionCodegenContext& context) const {
    if (!type.typeArguments().empty()) {
      return type.typeArguments();
    }
    const auto specialization = context.module.structSpecializationTypeArgs.find(type.structName());
    if (specialization == context.module.structSpecializationTypeArgs.end()) {
      return {};
    }
    return specialization->second;
  }

  LLVMGenerator::LLVMGenerator()
      : moduleState_(std::make_unique<ModuleState>(*this)),
        builtinsState_(std::make_unique<BuiltinsState>(*this)),
        expressionsState_(std::make_unique<ExpressionsState>(*this)),
        placesState_(std::make_unique<PlacesState>(*this)),
        statementsState_(std::make_unique<StatementsState>(*this)),
        structsState_(std::make_unique<StructsState>(*this)) {
    static_assert(std::derived_from<ModuleState, CodegenState>);
    static_assert(std::derived_from<BuiltinsState, CodegenState>);
    static_assert(std::derived_from<ExpressionsState, CodegenState>);
    static_assert(std::derived_from<PlacesState, CodegenState>);
    static_assert(std::derived_from<StatementsState, CodegenState>);
    static_assert(std::derived_from<StructsState, CodegenState>);
  }

  LLVMGenerator::~LLVMGenerator() = default;

  LLVMGenerator::LLVMGenerator(LLVMGenerator&& other) noexcept
      : functionSpecializationTypeArgs_(std::move(other.functionSpecializationTypeArgs_)),
        structSpecializationTypeArgs_(std::move(other.structSpecializationTypeArgs_)),
        moduleState_(std::move(other.moduleState_)),
        builtinsState_(std::move(other.builtinsState_)),
        expressionsState_(std::move(other.expressionsState_)),
        placesState_(std::move(other.placesState_)),
        statementsState_(std::move(other.statementsState_)),
        structsState_(std::move(other.structsState_)) {
    rebindStates();
  }

  LLVMGenerator& LLVMGenerator::operator=(LLVMGenerator&& other) noexcept {
    if (this != &other) {
      functionSpecializationTypeArgs_ = std::move(other.functionSpecializationTypeArgs_);
      structSpecializationTypeArgs_ = std::move(other.structSpecializationTypeArgs_);
      moduleState_ = std::move(other.moduleState_);
      builtinsState_ = std::move(other.builtinsState_);
      expressionsState_ = std::move(other.expressionsState_);
      placesState_ = std::move(other.placesState_);
      statementsState_ = std::move(other.statementsState_);
      structsState_ = std::move(other.structsState_);
      rebindStates();
    }
    return *this;
  }

  void LLVMGenerator::rebindStates() noexcept {
    moduleState_->rebind(*this);
    builtinsState_->rebind(*this);
    expressionsState_->rebind(*this);
    placesState_->rebind(*this);
    statementsState_->rebind(*this);
    structsState_->rebind(*this);
  }

  void LLVMGenerator::setFunctionSpecializationTypeArgs(
      std::unordered_map<std::string, std::vector<Type>> typeArgsByFunction) {
    functionSpecializationTypeArgs_ = std::move(typeArgsByFunction);
  }

  void LLVMGenerator::setStructSpecializationTypeArgs(
      std::unordered_map<std::string, std::vector<Type>> typeArgsByStruct) {
    structSpecializationTypeArgs_ = std::move(typeArgsByStruct);
  }

  std::string LLVMGenerator::generate(const ast::Module& module) const {
    return moduleState_->generateModule(module);
  }

  std::string LLVMGenerator::generateModule(const ast::Module& module) const {
    return moduleState_->generateModule(module);
  }

  bool LLVMGenerator::generateStatements(
      const std::vector<std::unique_ptr<ast::Statement>>& statements, IREmitter& emitter,
      FunctionCodegenContext& context, Type returnType, std::vector<Scope>& scopes) const {
    return statementsState_->generateStatements(statements, emitter, context, returnType, scopes);
  }

  bool LLVMGenerator::generateStatement(const ast::Statement& statement, IREmitter& emitter,
                                        FunctionCodegenContext& context, Type returnType,
                                        std::vector<Scope>& scopes) const {
    return statementsState_->generateStatement(statement, emitter, context, returnType, scopes);
  }

  std::string LLVMGenerator::generateCondition(const ast::Expression& expression,
                                               IREmitter& emitter,
                                               FunctionCodegenContext& context,
                                               const std::vector<Scope>& scopes) const {
    return expressionsState_->generateCondition(expression, emitter, context, scopes);
  }

  LLVMGenerator::Value LLVMGenerator::generateRvalue(const ast::Expression& expression,
                                                      IREmitter& emitter,
                                                      FunctionCodegenContext& context,
                                                      const std::vector<Scope>& scopes,
                                                      std::optional<Type> expectedType,
                                                      OwnershipMode ownership) const {
    return expressionsState_->generateRvalue(expression, emitter, context, scopes,
                                             std::move(expectedType), ownership);
  }

  LLVMGenerator::LocalBinding LLVMGenerator::generatePlace(
      const ast::Expression& expression, IREmitter& emitter, FunctionCodegenContext& context,
      const std::vector<Scope>& scopes) const {
    return placesState_->generatePlace(expression, emitter, context, scopes);
  }

  std::optional<LLVMGenerator::Value> LLVMGenerator::tryGenerateBuiltinCall(
      const ast::CallExpression& call, IREmitter& emitter, FunctionCodegenContext& context,
      const std::vector<Scope>& scopes) const {
    return builtinsState_->tryGenerateBuiltinCall(call, emitter, context, scopes);
  }

  LLVMGenerator::Value LLVMGenerator::generateStructLiteral(
      const ast::StructLiteral& literal, IREmitter& emitter, FunctionCodegenContext& context,
      const std::vector<Scope>& scopes) const {
    return structsState_->generateStructLiteral(literal, emitter, context, scopes);
  }

  LLVMGenerator::Value LLVMGenerator::generateFieldAccess(
      const ast::FieldAccessExpression& access, IREmitter& emitter,
      FunctionCodegenContext& context, const std::vector<Scope>& scopes) const {
    return structsState_->generateFieldAccess(access, emitter, context, scopes);
  }

  std::string LLVMGenerator::emitArrayElementPointer(
      const Value& base, const Value& index, const Type& elementType, IREmitter& emitter,
      FunctionCodegenContext& context) const {
    return placesState_->emitArrayElementPointer(base, index, elementType, emitter, context);
  }

  std::string LLVMGenerator::emitRawBufferElementPointer(const Value& base, const Value& index,
                                                          const Type& elementType,
                                                          IREmitter& emitter) const {
    return placesState_->emitRawBufferElementPointer(base, index, elementType, emitter);
  }

  std::string LLVMGenerator::emitBufferLoad(const Type& type, const std::string& pointer,
                                             IREmitter& emitter) const {
    return placesState_->emitBufferLoad(type, pointer, emitter);
  }

  void LLVMGenerator::emitBufferStore(const Type& type, const std::string& value,
                                      const std::string& pointer, IREmitter& emitter) const {
    placesState_->emitBufferStore(type, value, pointer, emitter);
  }

  std::string LLVMGenerator::emitCStringPointer(std::string_view text, IREmitter& emitter,
                                                 FunctionCodegenContext& context) const {
    return placesState_->emitCStringPointer(text, emitter, context);
  }

  void LLVMGenerator::emitRuntimeTrap(IREmitter& emitter, FunctionCodegenContext& context,
                                      std::string_view message) const {
    placesState_->emitRuntimeTrap(emitter, context, message);
  }

  void LLVMGenerator::emitTrapUnless(const std::string& condition, std::string_view label,
                                     IREmitter& emitter, FunctionCodegenContext& context,
                                     std::string_view message) const {
    placesState_->emitTrapUnless(condition, label, emitter, context, message);
  }

  void LLVMGenerator::emitNullPointerCheck(const std::string& pointer, IREmitter& emitter,
                                           FunctionCodegenContext& context) const {
    placesState_->emitNullPointerCheck(pointer, emitter, context);
  }

  std::string LLVMGenerator::emitCheckedMalloc(const std::string& size, IREmitter& emitter,
                                                FunctionCodegenContext& context) const {
    return placesState_->emitCheckedMalloc(size, emitter, context);
  }

  void LLVMGenerator::emitBoundsCheck(const std::string& length, const Value& index,
                                      IREmitter& emitter, FunctionCodegenContext& context,
                                      std::string_view label) const {
    placesState_->emitBoundsCheck(length, index, emitter, context, label);
  }

  LLVMGenerator::Value LLVMGenerator::emitDefaultValue(const Type& type, IREmitter& emitter,
                                                        FunctionCodegenContext& context) const {
    return moduleState_->emitDefaultValue(type, emitter, context);
  }

  void LLVMGenerator::emitDefaultStore(const Type& type, const std::string& slot,
                                       IREmitter& emitter,
                                       FunctionCodegenContext& context) const {
    moduleState_->emitDefaultStore(type, slot, emitter, context);
  }

  bool LLVMGenerator::declareLocal(std::vector<Scope>& scopes, const std::string& name,
                                   LocalBinding binding,
                                   FunctionCodegenContext& context) const {
    return statementsState_->declareLocal(scopes, name, std::move(binding), context);
  }

  const LLVMGenerator::LocalBinding&
  LLVMGenerator::lookupLocal(const std::vector<Scope>& scopes, const std::string& name) const {
    return statementsState_->lookupLocal(scopes, name);
  }

  bool LLVMGenerator::typeNeedsDrop(const Type& type,
                                     const FunctionCodegenContext& context) const {
    return placesState_->typeNeedsDrop(type, context);
  }

  bool LLVMGenerator::typeContainsManaged(const Type& type,
                                           const FunctionCodegenContext& context) const {
    return placesState_->typeContainsManaged(type, context);
  }

  void LLVMGenerator::emitDropScope(Scope& scope, IREmitter& emitter,
                                     FunctionCodegenContext& context) const {
    placesState_->emitDropScope(scope, emitter, context);
  }

  void LLVMGenerator::emitDropScopes(std::vector<Scope>& scopes, IREmitter& emitter,
                                      FunctionCodegenContext& context) const {
    placesState_->emitDropScopes(scopes, emitter, context);
  }

  void LLVMGenerator::emitDropValue(const Value& value, IREmitter& emitter,
                                     FunctionCodegenContext& context) const {
    placesState_->emitDropValue(value, emitter, context);
  }

  void LLVMGenerator::emitDropLocal(const LocalBinding& local, IREmitter& emitter,
                                     FunctionCodegenContext& context) const {
    placesState_->emitDropLocal(local, emitter, context);
  }

  LLVMGenerator::Value LLVMGenerator::emitCloneValue(const Value& value, IREmitter& emitter,
                                                      FunctionCodegenContext& context) const {
    return placesState_->emitCloneValue(value, emitter, context);
  }

  void LLVMGenerator::emitStoreManagedLocal(const LocalBinding& local, const Value& value,
                                             IREmitter& emitter,
                                             FunctionCodegenContext& context) const {
    placesState_->emitStoreManagedLocal(local, value, emitter, context);
  }

  void LLVMGenerator::emitReleaseIfOwned(const Value& value, IREmitter& emitter,
                                          FunctionCodegenContext& context) const {
    placesState_->emitReleaseIfOwned(value, emitter, context);
  }

  std::unordered_map<std::string, LLVMGenerator::FunctionBinding>
  LLVMGenerator::collectFunctionBindings(const ast::Module& module) const {
    return structsState_->collectFunctionBindings(module);
  }

  std::unordered_map<std::string, LLVMGenerator::StructLayout>
  LLVMGenerator::collectStructLayouts(const ast::Module& module) const {
    return structsState_->collectStructLayouts(module);
  }

  std::string LLVMGenerator::emitStructTypeDefinitions(const ast::Module& module) const {
    return structsState_->emitStructTypeDefinitions(module);
  }

  const LLVMGenerator::StructLayout&
  LLVMGenerator::lookupStructLayout(const FunctionCodegenContext& context,
                                    const Type& type) const {
    return structsState_->lookupStructLayout(context, type);
  }

  std::string LLVMGenerator::emitStructFieldPointer(const Type& type, const std::string& slot,
                                                     std::size_t index,
                                                     IREmitter& emitter) const {
    return structsState_->emitStructFieldPointer(type, slot, index, emitter);
  }

} // namespace noria
