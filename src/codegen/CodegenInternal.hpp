#pragma once

#include "noria/Ast.hpp"
#include "noria/AstVisitor.hpp"
#include "noria/Builtins.hpp"
#include "noria/Codegen.hpp"
#include "noria/Diagnostic.hpp"
#include "noria/HashTable.hpp"
#include "noria/IrEmitter.hpp"
#include "noria/SemanticTables.hpp"

#include "../internal/AstVisitorAdapters.hpp"

#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace noria::codegen_detail {

  struct Value {
    std::string text;
    Type type;
    bool owned = false;
  };

  struct LocalBinding {
    std::string slot;
    Type type;
    bool byteBuffer = false;
    std::string ownedSlot;
  };

  struct Scope {
    std::unordered_map<std::string, LocalBinding> bindings;
    bool containsPtr = false;
  };

  struct FunctionBinding {
    Type returnType;
    std::vector<Type> parameterTypes;
  };

  struct StructLayout {
    std::vector<std::string> fieldNames;
    std::vector<Type> fieldTypes;
    HashTable<std::string, std::size_t> fieldIndex;
  };

  struct ModuleCodegenContext {
    ModuleCodegenContext(const std::unordered_map<std::string, std::vector<Type>>& functionTypeArgs,
                         const std::unordered_map<std::string, std::vector<Type>>& structTypeArgs)
        : functionSpecializationTypeArgs(functionTypeArgs),
          structSpecializationTypeArgs(structTypeArgs) {}

    std::unordered_map<std::string, FunctionBinding> functions;
    std::unordered_map<std::string, StructLayout> structs;
    std::ostringstream globals;
    int nextStringGlobal = 0;
    const std::unordered_map<std::string, std::vector<Type>>& functionSpecializationTypeArgs;
    const std::unordered_map<std::string, std::vector<Type>>& structSpecializationTypeArgs;
  };

  struct FunctionCodegenContext {
    FunctionCodegenContext(ModuleCodegenContext& moduleContext, std::string functionName)
        : module(moduleContext), currentFunctionName(std::move(functionName)) {}

    ModuleCodegenContext& module;
    std::string currentFunctionName;
    std::vector<Scope> scopes;

    bool declareLocal(const std::string& name, LocalBinding binding, bool managed);
    const LocalBinding& lookupLocal(const std::string& name) const;
  };

  inline bool FunctionCodegenContext::declareLocal(const std::string& name, LocalBinding binding,
                                                   bool managed) {
    if (scopes.empty()) {
      scopes.emplace_back();
    }

    Scope& scope = scopes.back();
    if (scope.bindings.contains(name)) {
      return false;
    }
    if (managed) {
      scope.containsPtr = true;
    }
    scope.bindings.emplace(name, std::move(binding));
    return true;
  }

  inline const LocalBinding& FunctionCodegenContext::lookupLocal(const std::string& name) const {
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
      const auto local = scope->bindings.find(name);
      if (local != scope->bindings.end()) {
        return local->second;
      }
    }
    throw CompileError("codegen: unknown local variable '" + name + "'");
  }

  Value emitStandardContainerCall(StandardContainer, ContainerOperation, const std::vector<Type>&,
                                  const std::vector<Value>&, IREmitter&, FunctionCodegenContext&);
  std::vector<Type> specializedStructTypeArgs(const Type&, const FunctionCodegenContext&);

  class ExpressionEmitter;
  class OwnershipEmitter;
  class ModuleEmitter;
  class StatementEmitter;

  class MemoryEmitter {
  public:
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
  };

  class StructEmitter {
  public:
    Value generateStructLiteral(const ExpressionEmitter&, const OwnershipEmitter&,
                                const ast::StructLiteral&, IREmitter&,
                                FunctionCodegenContext&) const;
    Value generateFieldAccess(const ExpressionEmitter&, const ast::FieldAccessExpression&,
                              IREmitter&, FunctionCodegenContext&) const;
    std::unordered_map<std::string, StructLayout> collectStructLayouts(const ast::Module&) const;
    std::string emitStructTypeDefinitions(const ast::Module&) const;
    const StructLayout& lookupStructLayout(const FunctionCodegenContext&, const Type&) const;
    std::string emitStructFieldPointer(const Type&, const std::string&, std::size_t,
                                       IREmitter&) const;
  };

  class OwnershipEmitter {
  public:
    OwnershipEmitter(const MemoryEmitter& memory, const StructEmitter& structs)
        : memory_(memory), structs_(structs) {}

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
    const MemoryEmitter& memory_;
    const StructEmitter& structs_;
  };

  class BuiltinEmitter {
  public:
    BuiltinEmitter(const MemoryEmitter& memory, const OwnershipEmitter& ownership)
        : memory_(memory), ownership_(ownership) {}

    std::optional<Value> tryGenerateBuiltinCall(const ast::CallExpression&, IREmitter&,
                                                FunctionCodegenContext&,
                                                const ExpressionEmitter&) const;

  private:
    using BuiltinFn = Value (BuiltinEmitter::*)(const ast::CallExpression&, IREmitter&,
                                                FunctionCodegenContext&,
                                                const ExpressionEmitter&) const;

    std::optional<BuiltinFn> builtinEmitterFor(BuiltinId) const;
    Value emitPrintlnBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                             const ExpressionEmitter&) const;
    Value emitPrintBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                           const ExpressionEmitter&) const;
    Value emitPrintIntBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                              const ExpressionEmitter&) const;
    Value emitPrintFloatBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                                const ExpressionEmitter&) const;
    Value emitPrintCharBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                               const ExpressionEmitter&) const;
    Value emitSqrtBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                          const ExpressionEmitter&) const;
    Value emitPowBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                         const ExpressionEmitter&) const;
    Value emitLenBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                         const ExpressionEmitter&) const;
    Value emitRtAllocBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                             const ExpressionEmitter&) const;
    Value emitRtReallocBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                               const ExpressionEmitter&) const;
    Value emitRtReleaseBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                               const ExpressionEmitter&) const;
    Value emitRtSizeofBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                              const ExpressionEmitter&) const;
    Value emitRtLoadBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                            const ExpressionEmitter&) const;
    Value emitRtStoreBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                             const ExpressionEmitter&) const;
    Value emitRtDropBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                            const ExpressionEmitter&) const;
    Value emitRtLoadPtrBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                               const ExpressionEmitter&) const;
    Value emitRtStorePtrBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                                const ExpressionEmitter&) const;
    Value emitRtLoadI32Builtin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                               const ExpressionEmitter&) const;
    Value emitRtStoreI32Builtin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                                const ExpressionEmitter&) const;
    Value emitRtTrapBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                            const ExpressionEmitter&) const;
    Value emitRtNullBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                            const ExpressionEmitter&) const;
    Value emitRtPtrEqBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                             const ExpressionEmitter&) const;
    Value emitRtHashBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                            const ExpressionEmitter&) const;
    Value emitRtByteOffsetBuiltin(const ast::CallExpression&, IREmitter&, FunctionCodegenContext&,
                                  const ExpressionEmitter&) const;

    const MemoryEmitter& memory_;
    const OwnershipEmitter& ownership_;
  };

  class ExpressionEmitter {
  public:
    ExpressionEmitter(const MemoryEmitter& memory, const OwnershipEmitter& ownership,
                      const BuiltinEmitter& builtins, const StructEmitter& structs)
        : memory_(memory), ownership_(ownership), builtins_(builtins), structs_(structs) {}

    void setModule(const ModuleEmitter& module) { module_ = &module; }

    std::string generateCondition(const ast::Expression&, IREmitter&,
                                  FunctionCodegenContext&) const;
    Value generateRvalue(
        const ast::Expression&, IREmitter&, FunctionCodegenContext&,
        std::optional<Type> expectedType = std::nullopt,
        LLVMGenerator::OwnershipMode ownership = LLVMGenerator::OwnershipMode::Own) const;

  private:
    class ExpressionVisitor final : public internal::ExpressionOnlyVisitor {
    public:
      using internal::ExpressionOnlyVisitor::visit;
      ExpressionVisitor(const ExpressionEmitter&, IREmitter&, FunctionCodegenContext&,
                        std::optional<Type>, LLVMGenerator::OwnershipMode);
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
      const ExpressionEmitter& state_;
      IREmitter& emitter_;
      FunctionCodegenContext& context_;
      std::optional<Type> expectedType_;
      LLVMGenerator::OwnershipMode ownership_;
      Value result_{};
    };

    Value generateBinaryExpression(const ast::BinaryExpression&, IREmitter&,
                                   FunctionCodegenContext&) const;
    Value generateShortCircuitBinaryExpression(const ast::BinaryExpression&, IREmitter&,
                                               FunctionCodegenContext&) const;
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
    Value generateCastExpression(const ast::CastExpression&, IREmitter&,
                                 FunctionCodegenContext&) const;
    Value generateArrayLiteral(const ast::ArrayLiteral&, IREmitter&, FunctionCodegenContext&,
                               const std::optional<Type>&) const;
    Value generateIndexExpression(
        const ast::IndexExpression&, IREmitter&, FunctionCodegenContext&,
        LLVMGenerator::OwnershipMode ownership = LLVMGenerator::OwnershipMode::Own) const;
    Value emitCheckedF64ToI32Cast(const Value&, IREmitter&, FunctionCodegenContext&) const;
    const ModuleEmitter& module() const { return *module_; }

    const MemoryEmitter& memory_;
    const OwnershipEmitter& ownership_;
    const BuiltinEmitter& builtins_;
    const StructEmitter& structs_;
    const ModuleEmitter* module_ = nullptr;
  };

  class PlaceEmitter {
  public:
    PlaceEmitter(const ExpressionEmitter& expressions, const MemoryEmitter& memory,
                 const StructEmitter& structs)
        : expressions_(expressions), memory_(memory), structs_(structs) {}

    LocalBinding generatePlace(const ast::Expression&, IREmitter&, FunctionCodegenContext&) const;

  private:
    class PlaceVisitor final : public ast::AstVisitor {
    public:
      PlaceVisitor(const PlaceEmitter&, IREmitter&, FunctionCodegenContext&);
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
      const PlaceEmitter& state_;
      IREmitter& emitter_;
      FunctionCodegenContext& context_;
      LocalBinding result_{};
    };

    const ExpressionEmitter& expressions_;
    const MemoryEmitter& memory_;
    const StructEmitter& structs_;
  };

  class StatementEmitter {
  public:
    StatementEmitter(const ExpressionEmitter& expressions, const PlaceEmitter& places,
                     const OwnershipEmitter& ownership, const MemoryEmitter& memory)
        : expressions_(expressions), places_(places), ownership_(ownership), memory_(memory) {}

    void setModule(const ModuleEmitter& module) { module_ = &module; }

    bool generateStatements(const std::vector<std::unique_ptr<ast::Statement>>&, IREmitter&,
                            FunctionCodegenContext&, Type) const;
    bool generateStatement(const ast::Statement&, IREmitter&, FunctionCodegenContext&, Type) const;

  private:
    class StatementVisitor final : public internal::StatementOnlyVisitor {
    public:
      using internal::StatementOnlyVisitor::visit;
      StatementVisitor(const StatementEmitter&, IREmitter&, FunctionCodegenContext&, Type);
      bool returned() const { return returned_; }
      void visit(const ast::ReturnStatement&) override;
      void visit(const ast::LetStatement&) override;
      void visit(const ast::IfStatement&) override;
      void visit(const ast::WhileStatement&) override;
      void visit(const ast::AssignmentStatement&) override;
      void visit(const ast::ExpressionStatement&) override;

    private:
      const StatementEmitter& state_;
      IREmitter& emitter_;
      FunctionCodegenContext& context_;
      Type expectedReturnType_;
      bool returned_ = false;
    };

    void assignContainerIndex(const ast::IndexExpression&, const ast::Expression&, IREmitter&,
                              FunctionCodegenContext&) const;
    const ModuleEmitter& module() const { return *module_; }

    const ExpressionEmitter& expressions_;
    const PlaceEmitter& places_;
    const OwnershipEmitter& ownership_;
    const MemoryEmitter& memory_;
    const ModuleEmitter* module_ = nullptr;
  };

  class ModuleEmitter {
  public:
    ModuleEmitter(const StatementEmitter& statements, const MemoryEmitter& memory,
                  const OwnershipEmitter& ownership, const StructEmitter& structs)
        : statements_(statements), memory_(memory), ownership_(ownership), structs_(structs) {}

    std::string generateModule(const ast::Module&,
                               const std::unordered_map<std::string, std::vector<Type>>&,
                               const std::unordered_map<std::string, std::vector<Type>>&) const;
    Value emitDefaultValue(const Type&, IREmitter&, FunctionCodegenContext&) const;
    void emitDefaultStore(const Type&, const std::string&, IREmitter&,
                          FunctionCodegenContext&) const;
    std::unordered_map<std::string, FunctionBinding>
    collectFunctionBindings(const ast::Module&) const;

  private:
    std::string generateFunction(const ast::Function&, ModuleCodegenContext&) const;
    std::string modulePreamble() const;
    std::string defaultIRValue(const Type&) const;

    const StatementEmitter& statements_;
    const MemoryEmitter& memory_;
    const OwnershipEmitter& ownership_;
    const StructEmitter& structs_;
  };

} // namespace noria::codegen_detail

namespace noria {

  class LLVMGenerator::Impl {
  public:
    std::unordered_map<std::string, std::vector<Type>> functionSpecializationTypeArgs;
    std::unordered_map<std::string, std::vector<Type>> structSpecializationTypeArgs;
    codegen_detail::MemoryEmitter memory;
    codegen_detail::StructEmitter structs;
    codegen_detail::OwnershipEmitter ownership{memory, structs};
    codegen_detail::BuiltinEmitter builtins{memory, ownership};
    codegen_detail::ExpressionEmitter expressions{memory, ownership, builtins, structs};
    codegen_detail::PlaceEmitter places{expressions, memory, structs};
    codegen_detail::StatementEmitter statements{expressions, places, ownership, memory};
    codegen_detail::ModuleEmitter module{statements, memory, ownership, structs};

    Impl() {
      expressions.setModule(module);
      statements.setModule(module);
    }
  };

} // namespace noria
