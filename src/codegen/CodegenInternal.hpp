#pragma once

#include "noria/Codegen.hpp"

#include "../internal/AstVisitorAdapters.hpp"
#include "noria/AstVisitor.hpp"
#include "noria/Builtins.hpp"
#include "noria/HashTable.hpp"
#include "noria/IrEmitter.hpp"

#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace noria {

  class LLVMGenerator::Impl {
  public:
    void setFunctionSpecializationTypeArgs(
        std::unordered_map<std::string, std::vector<Type>> typeArgsByFunction) {
      functionSpecializationTypeArgs_ = std::move(typeArgsByFunction);
    }
    std::string generate(const ast::Module& module) const;

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

    using BuiltinEmitter = Value (Impl::*)(const ast::CallExpression&, IREmitter&,
                                           FunctionCodegenContext&,
                                           const std::vector<Scope>&) const;

    class StatementVisitor final : public internal::StatementOnlyVisitor {
    public:
      using internal::StatementOnlyVisitor::visit;

      StatementVisitor(const Impl& generator, IREmitter& emitter, FunctionCodegenContext& context,
                       Type expectedReturnType, std::vector<Scope>& scopes);

      bool returned() const { return returned_; }

      void visit(const ast::ReturnStatement& node) override;
      void visit(const ast::LetStatement& node) override;
      void visit(const ast::IfStatement& node) override;
      void visit(const ast::WhileStatement& node) override;
      void visit(const ast::AssignmentStatement& node) override;
      void visit(const ast::ExpressionStatement& node) override;

    private:
      const Impl& generator_;
      IREmitter& emitter_;
      FunctionCodegenContext& context_;
      Type expectedReturnType_;
      std::vector<Scope>& scopes_;
      bool returned_ = false;
    };

    class ExpressionVisitor final : public internal::ExpressionOnlyVisitor {
    public:
      using internal::ExpressionOnlyVisitor::visit;

      ExpressionVisitor(const Impl& generator, IREmitter& emitter, FunctionCodegenContext& context,
                        const std::vector<Scope>& scopes);

      Value result() const { return result_; }

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
      const Impl& generator_;
      IREmitter& emitter_;
      FunctionCodegenContext& context_;
      const std::vector<Scope>& scopes_;
      Value result_{};
    };

    class PlaceVisitor final : public ast::AstVisitor {
    public:
      PlaceVisitor(const Impl& generator, IREmitter& emitter, FunctionCodegenContext& context,
                   const std::vector<Scope>& scopes);

      LocalBinding result() const { return result_; }

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
      const Impl& generator_;
      IREmitter& emitter_;
      FunctionCodegenContext& context_;
      const std::vector<Scope>& scopes_;
      LocalBinding result_{};
    };

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
    std::string emitArrayElementPointer(const Value& base, const Value& indexValue,
                                        const Type& elementType, IREmitter& emitter,
                                        FunctionCodegenContext& context) const;
    std::string emitRawBufferElementPointer(const Value& base, const Value& indexValue,
                                            const Type& elementType, IREmitter& emitter) const;
    std::string emitBufferLoad(const Type& type, const std::string& pointer,
                               IREmitter& emitter) const;
    void emitBufferStore(const Type& type, const std::string& value, const std::string& pointer,
                         IREmitter& emitter) const;
    std::string emitCStringPointer(std::string_view text, IREmitter& emitter,
                                   FunctionCodegenContext& context) const;
    void emitRuntimeTrap(IREmitter& emitter, FunctionCodegenContext& context,
                         std::string_view message) const;
    void emitNullPointerCheck(const std::string& pointer, IREmitter& emitter,
                              FunctionCodegenContext& context) const;
    std::string emitCheckedMalloc(const std::string& size64, IREmitter& emitter,
                                  FunctionCodegenContext& context) const;
    void emitBoundsCheck(const std::string& length64, const Value& indexValue, IREmitter& emitter,
                         FunctionCodegenContext& context, std::string_view message) const;
    Value generateRvalue(const ast::Expression& expression, IREmitter& emitter,
                         FunctionCodegenContext& context, const std::vector<Scope>& scopes) const;

    Value generateBinaryExpression(const ast::BinaryExpression& binary, IREmitter& emitter,
                                   FunctionCodegenContext& context,
                                   const std::vector<Scope>& scopes) const;
    Value generateShortCircuitBinaryExpression(const ast::BinaryExpression& binary,
                                               IREmitter& emitter, FunctionCodegenContext& context,
                                               const std::vector<Scope>& scopes) const;
    Value generateStringConcatExpression(const Value& left, const Value& right, IREmitter& emitter,
                                         FunctionCodegenContext& context) const;
    Value generateComparisonExpression(const ast::BinaryExpression& binary, const Value& left,
                                       const Value& right, IREmitter& emitter) const;
    Value generateNumericBinaryExpression(const ast::BinaryExpression& binary, const Value& left,
                                          const Value& right, IREmitter& emitter) const;
    Value generateStringLiteral(const ast::StringLiteral& literal, IREmitter& emitter,
                                FunctionCodegenContext& context) const;
    Value generateCastExpression(const ast::CastExpression& cast, IREmitter& emitter,
                                 FunctionCodegenContext& context,
                                 const std::vector<Scope>& scopes) const;
    Value generateArrayLiteral(const ast::ArrayLiteral& literal, IREmitter& emitter,
                               FunctionCodegenContext& context,
                               const std::vector<Scope>& scopes) const;
    Value generateIndexExpression(const ast::IndexExpression& index, IREmitter& emitter,
                                  FunctionCodegenContext& context,
                                  const std::vector<Scope>& scopes) const;
    Value generateStructLiteral(const ast::StructLiteral& literal, IREmitter& emitter,
                                FunctionCodegenContext& context,
                                const std::vector<Scope>& scopes) const;
    Value generateFieldAccess(const ast::FieldAccessExpression& access, IREmitter& emitter,
                              FunctionCodegenContext& context,
                              const std::vector<Scope>& scopes) const;
    std::string emitStructFieldPointer(const Type& structType, const std::string& slot,
                                       std::size_t fieldIndex, IREmitter& emitter) const;
    std::optional<Value> tryGenerateBuiltinCall(const ast::CallExpression& call, IREmitter& emitter,
                                                FunctionCodegenContext& context,
                                                const std::vector<Scope>& scopes) const;
    std::optional<BuiltinEmitter> builtinEmitterFor(BuiltinId id) const;
    Value emitPrintlnBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                             FunctionCodegenContext& context,
                             const std::vector<Scope>& scopes) const;
    Value emitPrintBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                           FunctionCodegenContext& context, const std::vector<Scope>& scopes) const;
    Value emitPrintIntBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                              FunctionCodegenContext& context,
                              const std::vector<Scope>& scopes) const;
    Value emitPrintFloatBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                FunctionCodegenContext& context,
                                const std::vector<Scope>& scopes) const;
    Value emitPrintCharBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                               FunctionCodegenContext& context,
                               const std::vector<Scope>& scopes) const;
    Value emitSqrtBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                          FunctionCodegenContext& context, const std::vector<Scope>& scopes) const;
    Value emitPowBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                         FunctionCodegenContext& context, const std::vector<Scope>& scopes) const;
    Value emitLenBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                         FunctionCodegenContext& context, const std::vector<Scope>& scopes) const;
    Value emitRtAllocBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                             FunctionCodegenContext& context,
                             const std::vector<Scope>& scopes) const;
    Value emitRtReallocBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                               FunctionCodegenContext& context,
                               const std::vector<Scope>& scopes) const;
    Value emitRtReleaseBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                               FunctionCodegenContext& context,
                               const std::vector<Scope>& scopes) const;
    Value emitRtSizeofBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                              FunctionCodegenContext& context,
                              const std::vector<Scope>& scopes) const;
    Value emitRtLoadBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                            FunctionCodegenContext& context,
                            const std::vector<Scope>& scopes) const;
    Value emitRtStoreBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                             FunctionCodegenContext& context,
                             const std::vector<Scope>& scopes) const;
    Value emitRtLoadPtrBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                               FunctionCodegenContext& context,
                               const std::vector<Scope>& scopes) const;
    Value emitRtStorePtrBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                FunctionCodegenContext& context,
                                const std::vector<Scope>& scopes) const;
    Value emitRtLoadI32Builtin(const ast::CallExpression& call, IREmitter& emitter,
                               FunctionCodegenContext& context,
                               const std::vector<Scope>& scopes) const;
    Value emitRtStoreI32Builtin(const ast::CallExpression& call, IREmitter& emitter,
                                FunctionCodegenContext& context,
                                const std::vector<Scope>& scopes) const;
    Value emitRtTrapBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                            FunctionCodegenContext& context,
                            const std::vector<Scope>& scopes) const;
    Value emitRtNullBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                            FunctionCodegenContext& context,
                            const std::vector<Scope>& scopes) const;
    Value emitRtPtrEqBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                             FunctionCodegenContext& context,
                             const std::vector<Scope>& scopes) const;
    Value emitRtHashBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                            FunctionCodegenContext& context,
                            const std::vector<Scope>& scopes) const;
    Value emitRtByteOffsetBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                  FunctionCodegenContext& context,
                                  const std::vector<Scope>& scopes) const;

    std::string defaultIRValue(const Type& type) const;
    std::string modulePreamble() const;
    bool declareLocal(std::vector<Scope>& scopes, const std::string& name,
                      LocalBinding binding) const;
    const LocalBinding& lookupLocal(const std::vector<Scope>& scopes,
                                    const std::string& name) const;
    std::unordered_map<std::string, FunctionBinding>
    collectFunctionBindings(const ast::Module& module) const;
    std::unordered_map<std::string, StructLayout>
    collectStructLayouts(const ast::Module& module) const;
    std::string emitStructTypeDefinitions(const ast::Module& module) const;
    const StructLayout& lookupStructLayout(const FunctionCodegenContext& context,
                                           const Type& structType) const;

    std::unordered_map<std::string, std::vector<Type>> functionSpecializationTypeArgs_;
  };

} // namespace noria
