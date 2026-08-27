#pragma once

#include "noria/Codegen.hpp"

#include "../internal/AstVisitorAdapters.hpp"
#include "noria/AstVisitor.hpp"
#include "noria/Builtins.hpp"
#include "noria/SemanticTables.hpp"

#include <optional>
#include <vector>

namespace noria {

  class LLVMGenerator::CodegenState {
  public:
    explicit CodegenState(const LLVMGenerator& generator) : generator_(&generator) {}
    virtual ~CodegenState() = default;

    void rebind(const LLVMGenerator& generator) noexcept { generator_ = &generator; }

    Value emitStandardContainerCall(StandardContainer, ContainerOperation, const std::vector<Type>&,
                                    const std::vector<Value>&, IREmitter&,
                                    FunctionCodegenContext&) const;
    std::vector<Type> specializedStructTypeArgs(const Type&, const FunctionCodegenContext&) const;

  protected:
    const LLVMGenerator& generator() const { return *generator_; }

    std::string generateModule(const ast::Module& module) const {
      return generator().generateModule(module);
    }
    bool generateStatements(const std::vector<std::unique_ptr<ast::Statement>>& statements,
                            IREmitter& emitter, FunctionCodegenContext& context, Type returnType,
                            std::vector<Scope>& scopes) const {
      return generator().generateStatements(statements, emitter, context, returnType, scopes);
    }
    bool generateStatement(const ast::Statement& statement, IREmitter& emitter,
                           FunctionCodegenContext& context, Type returnType,
                           std::vector<Scope>& scopes) const {
      return generator().generateStatement(statement, emitter, context, returnType, scopes);
    }
    std::string generateCondition(const ast::Expression& expression, IREmitter& emitter,
                                  FunctionCodegenContext& context,
                                  const std::vector<Scope>& scopes) const {
      return generator().generateCondition(expression, emitter, context, scopes);
    }
    Value generateRvalue(const ast::Expression& expression, IREmitter& emitter,
                         FunctionCodegenContext& context, const std::vector<Scope>& scopes,
                         std::optional<Type> expectedType = std::nullopt,
                         LLVMGenerator::OwnershipMode ownership = LLVMGenerator::OwnershipMode::Own) const {
      return generator().generateRvalue(expression, emitter, context, scopes,
                                        std::move(expectedType), ownership);
    }
    LocalBinding generatePlace(const ast::Expression& expression, IREmitter& emitter,
                               FunctionCodegenContext& context,
                               const std::vector<Scope>& scopes) const {
      return generator().generatePlace(expression, emitter, context, scopes);
    }
    std::optional<Value> tryGenerateBuiltinCall(const ast::CallExpression& call, IREmitter& emitter,
                                                FunctionCodegenContext& context,
                                                const std::vector<Scope>& scopes) const {
      return generator().tryGenerateBuiltinCall(call, emitter, context, scopes);
    }
    Value generateStructLiteral(const ast::StructLiteral& literal, IREmitter& emitter,
                                FunctionCodegenContext& context,
                                const std::vector<Scope>& scopes) const {
      return generator().generateStructLiteral(literal, emitter, context, scopes);
    }
    Value generateFieldAccess(const ast::FieldAccessExpression& access, IREmitter& emitter,
                              FunctionCodegenContext& context,
                              const std::vector<Scope>& scopes) const {
      return generator().generateFieldAccess(access, emitter, context, scopes);
    }
    std::string emitArrayElementPointer(const Value& base, const Value& index,
                                        const Type& elementType, IREmitter& emitter,
                                        FunctionCodegenContext& context) const {
      return generator().emitArrayElementPointer(base, index, elementType, emitter, context);
    }
    std::string emitRawBufferElementPointer(const Value& base, const Value& index,
                                            const Type& elementType, IREmitter& emitter) const {
      return generator().emitRawBufferElementPointer(base, index, elementType, emitter);
    }
    std::string emitBufferLoad(const Type& type, const std::string& pointer,
                               IREmitter& emitter) const {
      return generator().emitBufferLoad(type, pointer, emitter);
    }
    void emitBufferStore(const Type& type, const std::string& value, const std::string& pointer,
                         IREmitter& emitter) const {
      generator().emitBufferStore(type, value, pointer, emitter);
    }
    std::string emitCStringPointer(std::string_view text, IREmitter& emitter,
                                   FunctionCodegenContext& context) const {
      return generator().emitCStringPointer(text, emitter, context);
    }
    void emitRuntimeTrap(IREmitter& emitter, FunctionCodegenContext& context,
                         std::string_view message) const {
      generator().emitRuntimeTrap(emitter, context, message);
    }
    void emitTrapUnless(const std::string& condition, std::string_view label, IREmitter& emitter,
                        FunctionCodegenContext& context, std::string_view message) const {
      generator().emitTrapUnless(condition, label, emitter, context, message);
    }
    void emitNullPointerCheck(const std::string& pointer, IREmitter& emitter,
                              FunctionCodegenContext& context) const {
      generator().emitNullPointerCheck(pointer, emitter, context);
    }
    std::string emitCheckedMalloc(const std::string& size, IREmitter& emitter,
                                  FunctionCodegenContext& context) const {
      return generator().emitCheckedMalloc(size, emitter, context);
    }
    void emitBoundsCheck(const std::string& length, const Value& index, IREmitter& emitter,
                         FunctionCodegenContext& context, std::string_view label) const {
      generator().emitBoundsCheck(length, index, emitter, context, label);
    }
    Value emitDefaultValue(const Type& type, IREmitter& emitter,
                           FunctionCodegenContext& context) const {
      return generator().emitDefaultValue(type, emitter, context);
    }
    void emitDefaultStore(const Type& type, const std::string& slot, IREmitter& emitter,
                          FunctionCodegenContext& context) const {
      generator().emitDefaultStore(type, slot, emitter, context);
    }
    bool declareLocal(std::vector<Scope>& scopes, const std::string& name,
                      LocalBinding binding, FunctionCodegenContext& context) const {
      return generator().declareLocal(scopes, name, std::move(binding), context);
    }
    const LocalBinding& lookupLocal(const std::vector<Scope>& scopes,
                                    const std::string& name) const {
      return generator().lookupLocal(scopes, name);
    }
    std::unordered_map<std::string, FunctionBinding>
    collectFunctionBindings(const ast::Module& module) const {
      return generator().collectFunctionBindings(module);
    }
    std::unordered_map<std::string, StructLayout>
    collectStructLayouts(const ast::Module& module) const {
      return generator().collectStructLayouts(module);
    }
    std::string emitStructTypeDefinitions(const ast::Module& module) const {
      return generator().emitStructTypeDefinitions(module);
    }
    const StructLayout& lookupStructLayout(const FunctionCodegenContext& context,
                                           const Type& type) const {
      return generator().lookupStructLayout(context, type);
    }
    std::string emitStructFieldPointer(const Type& type, const std::string& slot, std::size_t index,
                                       IREmitter& emitter) const {
      return generator().emitStructFieldPointer(type, slot, index, emitter);
    }

  private:
    const LLVMGenerator* generator_;
  };

  class LLVMGenerator::ModuleState final : public CodegenState {
  public:
    explicit ModuleState(const LLVMGenerator& generator) : CodegenState(generator) {}

    std::string generateModule(const ast::Module&) const;
    Value emitDefaultValue(const Type&, IREmitter&, FunctionCodegenContext&) const;
    void emitDefaultStore(const Type&, const std::string&, IREmitter&,
                          FunctionCodegenContext&) const;

  private:
    std::string generateFunction(const ast::Function&, ModuleCodegenContext&) const;
    std::string modulePreamble() const;
    std::string defaultIRValue(const Type&) const;
  };

  class LLVMGenerator::BuiltinsState final : public CodegenState {
  public:
    explicit BuiltinsState(const LLVMGenerator& generator) : CodegenState(generator) {}

    std::optional<Value> tryGenerateBuiltinCall(const ast::CallExpression&, IREmitter&,
                                                FunctionCodegenContext&,
                                                const std::vector<Scope>&) const;

  private:
    using BuiltinEmitter = Value (BuiltinsState::*)(const ast::CallExpression&, IREmitter&,
                                                    FunctionCodegenContext&,
                                                    const std::vector<Scope>&) const;
    std::optional<BuiltinEmitter> builtinEmitterFor(BuiltinId) const;
    Value emitPrintlnBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                             const std::vector<Scope>&) const;
    Value emitPrintBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                           const std::vector<Scope>&) const;
    Value emitPrintIntBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                              const std::vector<Scope>&) const;
    Value emitPrintFloatBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                                const std::vector<Scope>&) const;
    Value emitPrintCharBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                               const std::vector<Scope>&) const;
    Value emitSqrtBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                          const std::vector<Scope>&) const;
    Value emitPowBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                         const std::vector<Scope>&) const;
    Value emitLenBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                         const std::vector<Scope>&) const;
    Value emitRtAllocBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                             const std::vector<Scope>&) const;
    Value emitRtReallocBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                               const std::vector<Scope>&) const;
    Value emitRtReleaseBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                               const std::vector<Scope>&) const;
    Value emitRtSizeofBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                              const std::vector<Scope>&) const;
    Value emitRtLoadBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                            const std::vector<Scope>&) const;
    Value emitRtStoreBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                             const std::vector<Scope>&) const;
    Value emitRtLoadPtrBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                               const std::vector<Scope>&) const;
    Value emitRtStorePtrBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                                const std::vector<Scope>&) const;
    Value emitRtLoadI32Builtin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                               const std::vector<Scope>&) const;
    Value emitRtStoreI32Builtin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                                const std::vector<Scope>&) const;
    Value emitRtTrapBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                            const std::vector<Scope>&) const;
    Value emitRtNullBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                            const std::vector<Scope>&) const;
    Value emitRtPtrEqBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                             const std::vector<Scope>&) const;
    Value emitRtHashBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                            const std::vector<Scope>&) const;
    Value emitRtByteOffsetBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                                  const std::vector<Scope>&) const;
  };

  class LLVMGenerator::ExpressionsState final : public CodegenState {
  public:
    explicit ExpressionsState(const LLVMGenerator& generator) : CodegenState(generator) {}

    std::string generateCondition(const ast::Expression&, IREmitter&, FunctionCodegenContext&,
                                  const std::vector<Scope>&) const;
    Value generateRvalue(const ast::Expression&, IREmitter&, FunctionCodegenContext&,
                         const std::vector<Scope>&,
                         std::optional<Type> expectedType = std::nullopt,
                         LLVMGenerator::OwnershipMode ownership = LLVMGenerator::OwnershipMode::Own) const;

  private:
    class ExpressionVisitor final : public internal::ExpressionOnlyVisitor {
    public:
      using internal::ExpressionOnlyVisitor::visit;
      ExpressionVisitor(const ExpressionsState&, IREmitter&, FunctionCodegenContext&,
                        const std::vector<Scope>&, std::optional<Type>,
                        LLVMGenerator::OwnershipMode);
      Value result() const { return result_; }
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
      const ExpressionsState& state_;
      IREmitter& emitter_;
      FunctionCodegenContext& context_;
      const std::vector<Scope>& scopes_;
      std::optional<Type> expectedType_;
      LLVMGenerator::OwnershipMode ownership_;
      Value result_{};
    };

    Value generateBinaryExpression(const ast::BinaryExpression&, IREmitter&,
                                   FunctionCodegenContext&, const std::vector<Scope>&) const;
    Value generateShortCircuitBinaryExpression(const ast::BinaryExpression&, IREmitter&,
                                               FunctionCodegenContext&,
                                               const std::vector<Scope>&) const;
    Value generateStringConcatExpression(const Value&, const Value&, IREmitter&,
                                         FunctionCodegenContext&) const;
    Value generateCollectionAddExpression(const Value&, const Value&, IREmitter&,
                                          FunctionCodegenContext&) const;
    Value generateArrayAddExpression(const Value&, const Value&, IREmitter&,
                                     FunctionCodegenContext&) const;
    Value generateSequenceAddExpression(const Value&, const Value&, IREmitter&,
                                        FunctionCodegenContext&) const;
    Value generateElementAddExpression(const Value&, const Value&, IREmitter&,
                                       FunctionCodegenContext&) const;
    Value generateComparisonExpression(const ast::BinaryExpression&, const Value&, const Value&,
                                       IREmitter&) const;
    Value generateNumericBinaryExpression(const ast::BinaryExpression&, const Value&, const Value&,
                                          IREmitter&, FunctionCodegenContext&) const;
    Value generateStringLiteral(const ast::StringLiteral&, IREmitter&,
                                FunctionCodegenContext&) const;
    Value generateCastExpression(const ast::CastExpression&, IREmitter&, FunctionCodegenContext&,
                                 const std::vector<Scope>&) const;
    Value generateArrayLiteral(const ast::ArrayLiteral&, IREmitter&, FunctionCodegenContext&,
                               const std::vector<Scope>&, const std::optional<Type>&) const;
    Value generateIndexExpression(const ast::IndexExpression&, IREmitter&, FunctionCodegenContext&,
                                  const std::vector<Scope>&,
                                  LLVMGenerator::OwnershipMode ownership =
                                      LLVMGenerator::OwnershipMode::Own) const;
    Value emitCheckedF64ToI32Cast(const Value&, IREmitter&, FunctionCodegenContext&) const;
  };

  class LLVMGenerator::PlacesState final : public CodegenState {
  public:
    explicit PlacesState(const LLVMGenerator& generator) : CodegenState(generator) {}

    LocalBinding generatePlace(const ast::Expression&, IREmitter&, FunctionCodegenContext&,
                               const std::vector<Scope>&) const;
    std::string emitArrayElementPointer(const Value&, const Value&, const Type&, IREmitter&,
                                        FunctionCodegenContext&) const;
    std::string emitRawBufferElementPointer(const Value&, const Value&, const Type&,
                                            IREmitter&) const;
    std::string emitBufferLoad(const Type&, const std::string&, IREmitter&) const;
    void emitBufferStore(const Type&, const std::string&, const std::string&, IREmitter&) const;
    std::string emitCStringPointer(std::string_view, IREmitter&, FunctionCodegenContext&) const;
    void emitRuntimeTrap(IREmitter&, FunctionCodegenContext&, std::string_view) const;
    void emitTrapUnless(const std::string&, std::string_view, IREmitter&, FunctionCodegenContext&,
                        std::string_view) const;
    void emitNullPointerCheck(const std::string&, IREmitter&, FunctionCodegenContext&) const;
    std::string emitCheckedMalloc(const std::string&, IREmitter&, FunctionCodegenContext&) const;
    void emitBoundsCheck(const std::string&, const Value&, IREmitter&, FunctionCodegenContext&,
                         std::string_view) const;
    bool typeNeedsDrop(const Type&, const FunctionCodegenContext&) const;
    bool typeContainsManaged(const Type&, const FunctionCodegenContext&) const;
    void emitDropValue(const Value&, IREmitter&, FunctionCodegenContext&) const;
    void emitDropLocal(const LocalBinding&, IREmitter&, FunctionCodegenContext&) const;
    void emitDropScope(Scope&, IREmitter&, FunctionCodegenContext&) const;
    void emitDropScopes(std::vector<Scope>&, IREmitter&, FunctionCodegenContext&) const;
    Value emitCloneValue(const Value&, IREmitter&, FunctionCodegenContext&) const;
    void emitStoreManagedLocal(const LocalBinding&, const Value&, IREmitter&,
                               FunctionCodegenContext&) const;
    void emitReleaseIfOwned(const Value&, IREmitter&, FunctionCodegenContext&) const;

  private:
    class PlaceVisitor final : public ast::AstVisitor {
    public:
      PlaceVisitor(const PlacesState&, IREmitter&, FunctionCodegenContext&,
                   const std::vector<Scope>&);
      LocalBinding result() const { return result_; }
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
      const PlacesState& state_;
      IREmitter& emitter_;
      FunctionCodegenContext& context_;
      const std::vector<Scope>& scopes_;
      LocalBinding result_{};
    };
  };

  class LLVMGenerator::StatementsState final : public CodegenState {
  public:
    explicit StatementsState(const LLVMGenerator& generator) : CodegenState(generator) {}

    bool generateStatements(const std::vector<std::unique_ptr<ast::Statement>>& statements,
                            IREmitter&, FunctionCodegenContext&, Type, std::vector<Scope>&) const;
    bool generateStatement(const ast::Statement&, IREmitter&, FunctionCodegenContext&, Type,
                           std::vector<Scope>&) const;
    bool declareLocal(std::vector<Scope>&, const std::string&, LocalBinding,
                      FunctionCodegenContext&) const;
    const LocalBinding& lookupLocal(const std::vector<Scope>&, const std::string&) const;
    void emitDropScope(Scope&, IREmitter&, FunctionCodegenContext&) const;
    void emitDropScopes(std::vector<Scope>&, IREmitter&, FunctionCodegenContext&) const;

  private:
    class StatementVisitor final : public internal::StatementOnlyVisitor {
    public:
      using internal::StatementOnlyVisitor::visit;
      StatementVisitor(const StatementsState&, IREmitter&, FunctionCodegenContext&, Type,
                       std::vector<Scope>&);
      bool returned() const { return returned_; }
      void visit(const ast::ReturnStatement&) override;
      void visit(const ast::LetStatement&) override;
      void visit(const ast::IfStatement&) override;
      void visit(const ast::WhileStatement&) override;
      void visit(const ast::AssignmentStatement&) override;
      void visit(const ast::ExpressionStatement&) override;

    private:
      const StatementsState& state_;
      IREmitter& emitter_;
      FunctionCodegenContext& context_;
      Type expectedReturnType_;
      std::vector<Scope>& scopes_;
      bool returned_ = false;
    };

    void assignContainerIndex(const ast::IndexExpression&, const ast::Expression&, IREmitter&,
                              FunctionCodegenContext&, std::vector<Scope>&) const;
  };

  class LLVMGenerator::StructsState final : public CodegenState {
  public:
    explicit StructsState(const LLVMGenerator& generator) : CodegenState(generator) {}

    Value generateStructLiteral(const ast::StructLiteral&, IREmitter&, FunctionCodegenContext&,
                                const std::vector<Scope>&) const;
    Value generateFieldAccess(const ast::FieldAccessExpression&, IREmitter&,
                              FunctionCodegenContext&, const std::vector<Scope>&) const;
    std::unordered_map<std::string, FunctionBinding>
    collectFunctionBindings(const ast::Module&) const;
    std::unordered_map<std::string, StructLayout> collectStructLayouts(const ast::Module&) const;
    std::string emitStructTypeDefinitions(const ast::Module&) const;
    const StructLayout& lookupStructLayout(const FunctionCodegenContext&, const Type&) const;
    std::string emitStructFieldPointer(const Type&, const std::string&, std::size_t,
                                       IREmitter&) const;
  };

} // namespace noria
