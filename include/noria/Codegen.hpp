#pragma once

#include "noria/Ast.hpp"
#include "noria/AstVisitor.hpp"
#include "noria/Builtins.hpp"
#include "noria/IrEmitter.hpp"
#include "noria/Types.hpp"

#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>

namespace noria {

  class LLVMGenerator {
  public:
    void setFunctionSpecializationTypeArgs(
        std::unordered_map<std::string, std::vector<Type>> typeArgsByFunction) {
      functionSpecializationTypeArgs_ = std::move(typeArgsByFunction);
    }

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
      std::unordered_map<std::string, std::size_t> fieldIndex;
    };

    struct CodegenContext {
      std::unordered_map<std::string, FunctionBinding> functions;
      std::unordered_map<std::string, StructLayout> structs;
      std::ostringstream globals;
      int nextStringGlobal = 0;
      std::string currentFunctionName;
    };

    using Scope = std::unordered_map<std::string, LocalBinding>;
    using BuiltinEmitter = Value (LLVMGenerator::*)(const ast::CallExpression&, IREmitter&,
                                                    CodegenContext&,
                                                    const std::vector<Scope>&) const;

    class StatementVisitor final : public ast::AstVisitor {
    public:
      StatementVisitor(const LLVMGenerator& generator, IREmitter& emitter, CodegenContext& context,
                       Type expectedReturnType, std::vector<Scope>& scopes);

      bool returned() const { return returned_; }

      void visit(const ast::ReturnStatement& node) override;
      void visit(const ast::LetStatement& node) override;
      void visit(const ast::IfStatement& node) override;
      void visit(const ast::WhileStatement& node) override;
      void visit(const ast::AssignmentStatement& node) override;
      void visit(const ast::ExpressionStatement& node) override;

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
      const LLVMGenerator& generator_;
      IREmitter& emitter_;
      CodegenContext& context_;
      Type expectedReturnType_;
      std::vector<Scope>& scopes_;
      bool returned_ = false;
    };

    class ExpressionVisitor final : public ast::AstVisitor {
    public:
      ExpressionVisitor(const LLVMGenerator& generator, IREmitter& emitter, CodegenContext& context,
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

      void visit(const ast::ReturnStatement& node) override;
      void visit(const ast::LetStatement& node) override;
      void visit(const ast::IfStatement& node) override;
      void visit(const ast::WhileStatement& node) override;
      void visit(const ast::AssignmentStatement& node) override;
      void visit(const ast::ExpressionStatement& node) override;

    private:
      const LLVMGenerator& generator_;
      IREmitter& emitter_;
      CodegenContext& context_;
      const std::vector<Scope>& scopes_;
      Value result_{};
    };

    class PlaceVisitor final : public ast::AstVisitor {
    public:
      PlaceVisitor(const LLVMGenerator& generator, IREmitter& emitter, CodegenContext& context,
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
      const LLVMGenerator& generator_;
      IREmitter& emitter_;
      CodegenContext& context_;
      const std::vector<Scope>& scopes_;
      LocalBinding result_{};
    };

    std::string generateFunction(const ast::Function& function, CodegenContext& context) const;
    bool generateStatement(const ast::Statement& statement, IREmitter& emitter,
                           CodegenContext& context, Type expectedReturnType,
                           std::vector<Scope>& scopes) const;
    bool generateStatements(const std::vector<std::unique_ptr<ast::Statement>>& statements,
                            IREmitter& emitter, CodegenContext& context, Type expectedReturnType,
                            std::vector<Scope>& scopes) const;
    std::string generateCondition(const ast::Expression& expression, IREmitter& emitter,
                                  CodegenContext& context, const std::vector<Scope>& scopes) const;
    LocalBinding generatePlace(const ast::Expression& place, IREmitter& emitter,
                               CodegenContext& context, const std::vector<Scope>& scopes) const;
    std::string emitArrayElementPointer(const Value& base, const Value& indexValue,
                                        const Type& elementType, IREmitter& emitter,
                                        CodegenContext& context) const;
    std::string emitRawBufferElementPointer(const Value& base, const Value& indexValue,
                                            const Type& elementType, IREmitter& emitter) const;
    std::string emitBufferLoad(const Type& type, const std::string& pointer,
                               IREmitter& emitter) const;
    void emitBufferStore(const Type& type, const std::string& value, const std::string& pointer,
                         IREmitter& emitter) const;
    std::string emitCStringPointer(std::string_view text, IREmitter& emitter,
                                   CodegenContext& context) const;
    void emitRuntimeTrap(IREmitter& emitter, CodegenContext& context,
                         std::string_view message) const;
    void emitNullPointerCheck(const std::string& pointer, IREmitter& emitter,
                              CodegenContext& context) const;
    std::string emitCheckedMalloc(const std::string& size64, IREmitter& emitter,
                                  CodegenContext& context) const;
    void emitBoundsCheck(const std::string& length64, const Value& indexValue, IREmitter& emitter,
                         CodegenContext& context, std::string_view message) const;
    Value generateRvalue(const ast::Expression& expression, IREmitter& emitter,
                         CodegenContext& context, const std::vector<Scope>& scopes) const;

    Value generateBinaryExpression(const ast::BinaryExpression& binary, IREmitter& emitter,
                                   CodegenContext& context, const std::vector<Scope>& scopes) const;
    Value generateShortCircuitBinaryExpression(const ast::BinaryExpression& binary,
                                               IREmitter& emitter, CodegenContext& context,
                                               const std::vector<Scope>& scopes) const;
    Value generateStringConcatExpression(const Value& left, const Value& right, IREmitter& emitter,
                                         CodegenContext& context) const;
    Value generateComparisonExpression(const ast::BinaryExpression& binary, const Value& left,
                                       const Value& right, IREmitter& emitter) const;
    Value generateNumericBinaryExpression(const ast::BinaryExpression& binary, const Value& left,
                                          const Value& right, IREmitter& emitter) const;
    Value generateStringLiteral(const ast::StringLiteral& literal, IREmitter& emitter,
                                CodegenContext& context) const;
    Value generateCastExpression(const ast::CastExpression& cast, IREmitter& emitter,
                                 CodegenContext& context, const std::vector<Scope>& scopes) const;
    Value generateArrayLiteral(const ast::ArrayLiteral& literal, IREmitter& emitter,
                               CodegenContext& context, const std::vector<Scope>& scopes) const;
    Value generateIndexExpression(const ast::IndexExpression& index, IREmitter& emitter,
                                  CodegenContext& context, const std::vector<Scope>& scopes) const;
    Value generateStructLiteral(const ast::StructLiteral& literal, IREmitter& emitter,
                                CodegenContext& context, const std::vector<Scope>& scopes) const;
    Value generateFieldAccess(const ast::FieldAccessExpression& access, IREmitter& emitter,
                              CodegenContext& context, const std::vector<Scope>& scopes) const;
    std::string emitStructFieldPointer(const Type& structType, const std::string& slot,
                                       std::size_t fieldIndex, IREmitter& emitter) const;
    std::optional<Value> tryGenerateBuiltinCall(const ast::CallExpression& call, IREmitter& emitter,
                                                CodegenContext& context,
                                                const std::vector<Scope>& scopes) const;
    std::optional<BuiltinEmitter> builtinEmitterFor(BuiltinId id) const;
    Value emitPrintlnBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                             CodegenContext& context, const std::vector<Scope>& scopes) const;
    Value emitPrintBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                           CodegenContext& context, const std::vector<Scope>& scopes) const;
    Value emitPrintIntBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                              CodegenContext& context, const std::vector<Scope>& scopes) const;
    Value emitPrintFloatBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                CodegenContext& context, const std::vector<Scope>& scopes) const;
    Value emitPrintCharBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                               CodegenContext& context, const std::vector<Scope>& scopes) const;
    Value emitSqrtBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                          CodegenContext& context, const std::vector<Scope>& scopes) const;
    Value emitPowBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                         CodegenContext& context, const std::vector<Scope>& scopes) const;
    Value emitLenBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                         CodegenContext& context, const std::vector<Scope>& scopes) const;
    Value emitRtAllocBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                             CodegenContext& context, const std::vector<Scope>& scopes) const;
    Value emitRtReallocBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                               CodegenContext& context, const std::vector<Scope>& scopes) const;
    Value emitRtReleaseBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                               CodegenContext& context, const std::vector<Scope>& scopes) const;
    Value emitRtSizeofBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                              CodegenContext& context, const std::vector<Scope>& scopes) const;
    Value emitRtLoadBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                            CodegenContext& context, const std::vector<Scope>& scopes) const;
    Value emitRtStoreBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                             CodegenContext& context, const std::vector<Scope>& scopes) const;
    Value emitRtLoadPtrBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                               CodegenContext& context, const std::vector<Scope>& scopes) const;
    Value emitRtStorePtrBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                CodegenContext& context, const std::vector<Scope>& scopes) const;
    Value emitRtLoadI32Builtin(const ast::CallExpression& call, IREmitter& emitter,
                               CodegenContext& context, const std::vector<Scope>& scopes) const;
    Value emitRtStoreI32Builtin(const ast::CallExpression& call, IREmitter& emitter,
                                CodegenContext& context, const std::vector<Scope>& scopes) const;
    Value emitRtTrapBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                            CodegenContext& context, const std::vector<Scope>& scopes) const;
    Value emitRtNullBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                            CodegenContext& context, const std::vector<Scope>& scopes) const;
    Value emitRtPtrEqBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                             CodegenContext& context, const std::vector<Scope>& scopes) const;
    Value emitRtHashBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                            CodegenContext& context, const std::vector<Scope>& scopes) const;
    Value emitRtByteOffsetBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                  CodegenContext& context,
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
    const StructLayout& lookupStructLayout(const CodegenContext& context,
                                           const Type& structType) const;

    mutable std::unordered_map<std::string, std::vector<Type>> functionSpecializationTypeArgs_;
  };

} // namespace noria
