#pragma once

#include "noria/Ast.hpp"
#include "noria/HashTable.hpp"
#include "noria/IrEmitter.hpp"
#include "noria/Types.hpp"

#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace noria {

  class LLVMGenerator {
  public:
    LLVMGenerator();
    ~LLVMGenerator();
    LLVMGenerator(const LLVMGenerator&) = delete;
    LLVMGenerator& operator=(const LLVMGenerator&) = delete;
    LLVMGenerator(LLVMGenerator&& other) noexcept;
    LLVMGenerator& operator=(LLVMGenerator&& other) noexcept;

    void setFunctionSpecializationTypeArgs(
        std::unordered_map<std::string, std::vector<Type>> typeArgsByFunction);
    void setStructSpecializationTypeArgs(
        std::unordered_map<std::string, std::vector<Type>> typeArgsByStruct);
    std::string generate(const ast::Module& module) const;

    enum class OwnershipMode {
      Borrow,
      Own,
    };

  private:

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
      ModuleCodegenContext(
          const std::unordered_map<std::string, std::vector<Type>>& functionTypeArgs,
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
          : module(moduleContext), currentFunctionName(std::move(functionName)),
            functions(module.functions), structs(module.structs), globals(module.globals),
            nextStringGlobal(module.nextStringGlobal) {}
      ModuleCodegenContext& module;
      std::string currentFunctionName;
      std::vector<Scope> scopes;
      std::unordered_map<std::string, FunctionBinding>& functions;
      std::unordered_map<std::string, StructLayout>& structs;
      std::ostringstream& globals;
      int& nextStringGlobal;
      const std::unordered_map<std::string, std::vector<Type>>& functionSpecializationTypeArgs =
          module.functionSpecializationTypeArgs;
    };

    class CodegenState;
    class ModuleState;
    class BuiltinsState;
    class ExpressionsState;
    class PlacesState;
    class StatementsState;
    class StructsState;

    void rebindStates() noexcept;

    std::string generateModule(const ast::Module&) const;
    bool generateStatements(const std::vector<std::unique_ptr<ast::Statement>>&, IREmitter&,
                            FunctionCodegenContext&, Type, std::vector<Scope>&) const;
    bool generateStatement(const ast::Statement&, IREmitter&, FunctionCodegenContext&, Type,
                           std::vector<Scope>&) const;
    std::string generateCondition(const ast::Expression&, IREmitter&, FunctionCodegenContext&,
                                  const std::vector<Scope>&) const;
    Value generateRvalue(const ast::Expression&, IREmitter&, FunctionCodegenContext&,
                         const std::vector<Scope>&,
                         std::optional<Type> expectedType = std::nullopt,
                         OwnershipMode ownership = OwnershipMode::Own) const;
    LocalBinding generatePlace(const ast::Expression&, IREmitter&, FunctionCodegenContext&,
                               const std::vector<Scope>&) const;
    std::optional<Value> tryGenerateBuiltinCall(const ast::CallExpression&, IREmitter&,
                                                FunctionCodegenContext&,
                                                const std::vector<Scope>&) const;
    Value generateStructLiteral(const ast::StructLiteral&, IREmitter&, FunctionCodegenContext&,
                                const std::vector<Scope>&) const;
    Value generateFieldAccess(const ast::FieldAccessExpression&, IREmitter&,
                              FunctionCodegenContext&, const std::vector<Scope>&) const;
    std::string emitArrayElementPointer(const Value&, const Value&, const Type&, IREmitter&,
                                        FunctionCodegenContext&) const;
    std::string emitRawBufferElementPointer(const Value&, const Value&, const Type&,
                                            IREmitter&) const;
    std::string emitBufferLoad(const Type&, const std::string&, IREmitter&) const;
    void emitBufferStore(const Type&, const std::string&, const std::string&, IREmitter&) const;
    std::string emitCStringPointer(std::string_view, IREmitter&, FunctionCodegenContext&) const;
    void emitRuntimeTrap(IREmitter&, FunctionCodegenContext&, std::string_view) const;
    void emitTrapUnless(const std::string&, std::string_view, IREmitter&,
                        FunctionCodegenContext&, std::string_view) const;
    void emitNullPointerCheck(const std::string&, IREmitter&, FunctionCodegenContext&) const;
    std::string emitCheckedMalloc(const std::string&, IREmitter&, FunctionCodegenContext&) const;
    void emitBoundsCheck(const std::string&, const Value&, IREmitter&, FunctionCodegenContext&,
                         std::string_view) const;
    Value emitDefaultValue(const Type&, IREmitter&, FunctionCodegenContext&) const;
    void emitDefaultStore(const Type&, const std::string&, IREmitter&,
                          FunctionCodegenContext&) const;
    bool declareLocal(std::vector<Scope>&, const std::string&, LocalBinding,
                      FunctionCodegenContext&) const;
    const LocalBinding& lookupLocal(const std::vector<Scope>&, const std::string&) const;
    void emitDropScope(Scope&, IREmitter&, FunctionCodegenContext&) const;
    void emitDropScopes(std::vector<Scope>&, IREmitter&, FunctionCodegenContext&) const;
    void emitDropValue(const Value&, IREmitter&, FunctionCodegenContext&) const;
    void emitDropLocal(const LocalBinding&, IREmitter&, FunctionCodegenContext&) const;
    Value emitCloneValue(const Value&, IREmitter&, FunctionCodegenContext&) const;
    void emitStoreManagedLocal(const LocalBinding&, const Value&, IREmitter&,
                               FunctionCodegenContext&) const;
    void emitReleaseIfOwned(const Value&, IREmitter&, FunctionCodegenContext&) const;
    bool typeNeedsDrop(const Type&, const FunctionCodegenContext&) const;
    bool typeContainsManaged(const Type&, const FunctionCodegenContext&) const;
    std::unordered_map<std::string, FunctionBinding>
    collectFunctionBindings(const ast::Module&) const;
    std::unordered_map<std::string, StructLayout>
    collectStructLayouts(const ast::Module&) const;
    std::string emitStructTypeDefinitions(const ast::Module&) const;
    const StructLayout& lookupStructLayout(const FunctionCodegenContext&, const Type&) const;
    std::string emitStructFieldPointer(const Type&, const std::string&, std::size_t,
                                       IREmitter&) const;

    std::unordered_map<std::string, std::vector<Type>> functionSpecializationTypeArgs_;
    std::unordered_map<std::string, std::vector<Type>> structSpecializationTypeArgs_;
    std::unique_ptr<ModuleState> moduleState_;
    std::unique_ptr<BuiltinsState> builtinsState_;
    std::unique_ptr<ExpressionsState> expressionsState_;
    std::unique_ptr<PlacesState> placesState_;
    std::unique_ptr<StatementsState> statementsState_;
    std::unique_ptr<StructsState> structsState_;
  };

} // namespace noria
