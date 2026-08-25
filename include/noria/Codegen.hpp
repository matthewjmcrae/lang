#pragma once

#include "noria/Ast.hpp"
#include "noria/Builtins.hpp"
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

  class CodegenStrategy;
  enum class CodegenStrategyKind;
  class CodegenModule;
  class CodegenBuiltins;
  class CodegenExpressions;
  class CodegenPlaces;
  class CodegenStatements;
  class CodegenStructs;

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
    std::string generate(const ast::Module& module) const;

  private:
    struct Value {
      std::string text;
      Type type;
    };
    struct LocalBinding {
      std::string slot;
      Type type;
      bool byteBuffer = false;
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
    using Scope = std::unordered_map<std::string, LocalBinding>;

    struct ModuleCodegenContext {
      explicit ModuleCodegenContext(
          const std::unordered_map<std::string, std::vector<Type>>& specializationTypeArgs)
          : functionSpecializationTypeArgs(specializationTypeArgs) {}
      std::unordered_map<std::string, FunctionBinding> functions;
      std::unordered_map<std::string, StructLayout> structs;
      std::ostringstream globals;
      int nextStringGlobal = 0;
      const std::unordered_map<std::string, std::vector<Type>>& functionSpecializationTypeArgs;
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

    using BuiltinEmitter = Value (LLVMGenerator::*)(const ast::CallExpression&, IREmitter&,
                                                    FunctionCodegenContext&,
                                                    const std::vector<Scope>&) const;
    class StatementVisitor;
    class ExpressionVisitor;
    class PlaceVisitor;

    class StrategyScope {
    public:
      StrategyScope(const LLVMGenerator& generator, CodegenStrategyKind requested);
      ~StrategyScope();
      StrategyScope(const StrategyScope&) = delete;
      StrategyScope& operator=(const StrategyScope&) = delete;

    private:
      const LLVMGenerator& generator_;
      std::unique_ptr<CodegenStrategy> previous_;
    };
    StrategyScope activate(CodegenStrategyKind requested) const;

    std::string generateModule(const ast::Module& module) const;
    std::string generateFunction(const ast::Function& function,
                                 ModuleCodegenContext& context) const;
    bool generateStatement(const ast::Statement& statement, IREmitter& emitter,
                           FunctionCodegenContext& context, Type expectedReturnType,
                           std::vector<Scope>& scopes) const;
    bool generateStatements(const std::vector<std::unique_ptr<ast::Statement>>& statements,
                            IREmitter& emitter, FunctionCodegenContext& context,
                            Type expectedReturnType, std::vector<Scope>& scopes) const;
    std::string generateCondition(const ast::Expression& expression, IREmitter& emitter,
                                  FunctionCodegenContext& context,
                                  const std::vector<Scope>& scopes) const;
    LocalBinding generatePlace(const ast::Expression& place, IREmitter& emitter,
                               FunctionCodegenContext& context,
                               const std::vector<Scope>& scopes) const;
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
    Value emitCheckedF64ToI32Cast(const Value&, IREmitter&, FunctionCodegenContext&) const;
    Value generateRvalue(const ast::Expression&, IREmitter&, FunctionCodegenContext&,
                         const std::vector<Scope>&,
                         std::optional<Type> expectedType = std::nullopt) const;
    Value generateBinaryExpression(const ast::BinaryExpression&, IREmitter&,
                                   FunctionCodegenContext&, const std::vector<Scope>&) const;
    Value generateShortCircuitBinaryExpression(const ast::BinaryExpression&, IREmitter&,
                                               FunctionCodegenContext&,
                                               const std::vector<Scope>&) const;
    Value generateStringConcatExpression(const Value&, const Value&, IREmitter&,
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
                                  const std::vector<Scope>&) const;
    Value generateStructLiteral(const ast::StructLiteral&, IREmitter&, FunctionCodegenContext&,
                                const std::vector<Scope>&) const;
    Value generateFieldAccess(const ast::FieldAccessExpression&, IREmitter&,
                              FunctionCodegenContext&, const std::vector<Scope>&) const;
    std::string emitStructFieldPointer(const Type&, const std::string&, std::size_t,
                                       IREmitter&) const;
    std::optional<Value> tryGenerateBuiltinCall(const ast::CallExpression&, IREmitter&,
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
    Value emitDefaultValue(const Type&, IREmitter&, FunctionCodegenContext&) const;
    void emitDefaultStore(const Type&, const std::string& slot, IREmitter&,
                          FunctionCodegenContext&) const;
    std::string defaultIRValue(const Type&) const;
    std::string modulePreamble() const;
    bool declareLocal(std::vector<Scope>&, const std::string&, LocalBinding) const;
    const LocalBinding& lookupLocal(const std::vector<Scope>&, const std::string&) const;
    std::unordered_map<std::string, FunctionBinding>
    collectFunctionBindings(const ast::Module&) const;
    std::unordered_map<std::string, StructLayout> collectStructLayouts(const ast::Module&) const;
    std::string emitStructTypeDefinitions(const ast::Module&) const;
    const StructLayout& lookupStructLayout(const FunctionCodegenContext&, const Type&) const;

    std::unordered_map<std::string, std::vector<Type>> functionSpecializationTypeArgs_;
    mutable std::unique_ptr<CodegenStrategy> activeStrategy_;
    friend class CodegenStrategy;
    friend class CodegenModule;
    friend class CodegenBuiltins;
    friend class CodegenExpressions;
    friend class CodegenPlaces;
    friend class CodegenStatements;
    friend class CodegenStructs;
  };

} // namespace noria
