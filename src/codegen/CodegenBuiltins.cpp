#include "CodegenInternal.hpp"
#include "CodegenStrategy.hpp"

#include "noria/Builtins.hpp"
#include "noria/Diagnostic.hpp"
#include "noria/Runtime.hpp"
#include "noria/SemanticTables.hpp"

#include "CodegenSupport.hpp"
#include <array>
#include <charconv>
#include <limits>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace noria {

  using namespace codegen_detail;

  std::optional<LLVMGenerator::Value>
  LLVMGenerator::tryGenerateBuiltinCall(const ast::CallExpression& call, IREmitter& emitter,
                                              FunctionCodegenContext& context,
                                              const std::vector<Scope>& scopes) const {
    const auto strategy = activate(CodegenStrategyKind::Builtins);

    const BuiltinSignature* descriptor = lookupBuiltin(call.callee);
    if (descriptor == nullptr)
      return std::nullopt;

    if (auto builtinEmitter = builtinEmitterFor(descriptor->id)) {
      return (this->*(*builtinEmitter))(call, emitter, context, scopes);
    }

    return std::nullopt;
  }

  std::optional<LLVMGenerator::BuiltinEmitter>
  LLVMGenerator::builtinEmitterFor(BuiltinId id) const {
    static constexpr std::array<std::pair<BuiltinId, BuiltinEmitter>, 23> emitters{{
        {BuiltinId::Println, &LLVMGenerator::emitPrintlnBuiltin},
        {BuiltinId::Print, &LLVMGenerator::emitPrintBuiltin},
        {BuiltinId::PrintInt, &LLVMGenerator::emitPrintIntBuiltin},
        {BuiltinId::PrintFloat, &LLVMGenerator::emitPrintFloatBuiltin},
        {BuiltinId::PrintChar, &LLVMGenerator::emitPrintCharBuiltin},
        {BuiltinId::Sqrt, &LLVMGenerator::emitSqrtBuiltin},
        {BuiltinId::Pow, &LLVMGenerator::emitPowBuiltin},
        {BuiltinId::Len, &LLVMGenerator::emitLenBuiltin},
        {BuiltinId::RtAlloc, &LLVMGenerator::emitRtAllocBuiltin},
        {BuiltinId::RtRealloc, &LLVMGenerator::emitRtReallocBuiltin},
        {BuiltinId::RtRelease, &LLVMGenerator::emitRtReleaseBuiltin},
        {BuiltinId::RtSizeof, &LLVMGenerator::emitRtSizeofBuiltin},
        {BuiltinId::RtLoad, &LLVMGenerator::emitRtLoadBuiltin},
        {BuiltinId::RtStore, &LLVMGenerator::emitRtStoreBuiltin},
        {BuiltinId::RtLoadPtr, &LLVMGenerator::emitRtLoadPtrBuiltin},
        {BuiltinId::RtStorePtr, &LLVMGenerator::emitRtStorePtrBuiltin},
        {BuiltinId::RtLoadI32, &LLVMGenerator::emitRtLoadI32Builtin},
        {BuiltinId::RtStoreI32, &LLVMGenerator::emitRtStoreI32Builtin},
        {BuiltinId::RtTrap, &LLVMGenerator::emitRtTrapBuiltin},
        {BuiltinId::RtNull, &LLVMGenerator::emitRtNullBuiltin},
        {BuiltinId::RtPtrEq, &LLVMGenerator::emitRtPtrEqBuiltin},
        {BuiltinId::RtHash, &LLVMGenerator::emitRtHashBuiltin},
        {BuiltinId::RtByteOffset, &LLVMGenerator::emitRtByteOffsetBuiltin},
    }};

    for (const auto& [candidate, emitter] : emitters) {
      if (candidate == id) {
        return emitter;
      }
    }
    return std::nullopt;
  }

  LLVMGenerator::Value
  LLVMGenerator::emitPrintlnBuiltin(const ast::CallExpression&, IREmitter& emitter,
                                          FunctionCodegenContext&,
                                          const std::vector<Scope>&) const {
    emitter.line("call i32 @putchar(i32 10)");
    return Value{"", Type::voidType()};
  }

  LLVMGenerator::Value
  LLVMGenerator::emitPrintBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                        FunctionCodegenContext& context,
                                        const std::vector<Scope>& scopes) const {
    const Value argument = generateRvalue(*call.arguments[0], emitter, context, scopes);
    const std::string formatPointer = emitter.freshTemp();
    emitter.line(formatPointer +
                 " = getelementptr inbounds [3 x i8], ptr @.fmt.str, i32 0, i32 0");
    emitter.line("call i32 (ptr, ...) @printf(ptr " + formatPointer + ", ptr " + argument.text +
                 ")");
    return Value{"", Type::voidType()};
  }

  LLVMGenerator::Value
  LLVMGenerator::emitPrintIntBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                           FunctionCodegenContext& context,
                                           const std::vector<Scope>& scopes) const {
    const Value argument = generateRvalue(*call.arguments[0], emitter, context, scopes);
    emitter.line("call void @noria_print_int(i32 " + argument.text + ")");
    return Value{"", Type::voidType()};
  }

  LLVMGenerator::Value
  LLVMGenerator::emitPrintFloatBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                             FunctionCodegenContext& context,
                                             const std::vector<Scope>& scopes) const {
    const Value argument = generateRvalue(*call.arguments[0], emitter, context, scopes);
    const std::string formatPointer = emitter.freshTemp();
    emitter.line(formatPointer +
                 " = getelementptr inbounds [4 x i8], ptr @.fmt.float, i32 0, i32 0");
    emitter.line("call i32 (ptr, ...) @printf(ptr " + formatPointer + ", double " + argument.text +
                 ")");
    return Value{"", Type::voidType()};
  }

  LLVMGenerator::Value
  LLVMGenerator::emitPrintCharBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                            FunctionCodegenContext& context,
                                            const std::vector<Scope>& scopes) const {
    const Value argument = generateRvalue(*call.arguments[0], emitter, context, scopes);
    emitter.line("call i32 @putchar(i32 " + argument.text + ")");
    return Value{"", Type::voidType()};
  }

  LLVMGenerator::Value
  LLVMGenerator::emitSqrtBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                       FunctionCodegenContext& context,
                                       const std::vector<Scope>& scopes) const {
    const Value argument = generateRvalue(*call.arguments[0], emitter, context, scopes);
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = call double @llvm.sqrt.f64(double " + argument.text + ")");
    return Value{result, Type::f64()};
  }

  LLVMGenerator::Value
  LLVMGenerator::emitPowBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                      FunctionCodegenContext& context,
                                      const std::vector<Scope>& scopes) const {
    const Value base = generateRvalue(*call.arguments[0], emitter, context, scopes);
    const Value exponent = generateRvalue(*call.arguments[1], emitter, context, scopes);
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = call double @llvm.pow.f64(double " + base.text + ", double " +
                 exponent.text + ")");
    return Value{result, Type::f64()};
  }

  LLVMGenerator::Value
  LLVMGenerator::emitLenBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                      FunctionCodegenContext& context,
                                      const std::vector<Scope>& scopes) const {
    const Value argument = generateRvalue(*call.arguments[0], emitter, context, scopes);
    if (argument.type == Type::str()) {
      const std::string length = emitter.freshTemp();
      emitter.line(length + " = call i64 @strlen(ptr " + argument.text + ")");
      const std::string result = emitter.freshTemp();
      emitter.line(result + " = trunc i64 " + length + " to i32");
      return Value{result, Type::i32()};
    }

    const std::string length = emitter.freshTemp();
    emitter.line(length + " = load i64, ptr " + argument.text);
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = trunc i64 " + length + " to i32");
    return Value{result, Type::i32()};
  }

  LLVMGenerator::Value
  LLVMGenerator::emitRtAllocBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                          FunctionCodegenContext& context,
                                          const std::vector<Scope>& scopes) const {
    const Value size = generateRvalue(*call.arguments[0], emitter, context, scopes);
    const std::string size64 = emitter.freshTemp();
    emitter.line(size64 + " = sext i32 " + size.text + " to i64");
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = call ptr @malloc(i64 " + size64 + ")");
    emitNullPointerCheck(result, emitter, context);
    return Value{result, Type::rawPtr()};
  }

  LLVMGenerator::Value
  LLVMGenerator::emitRtReallocBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                            FunctionCodegenContext& context,
                                            const std::vector<Scope>& scopes) const {
    const Value pointer = generateRvalue(*call.arguments[0], emitter, context, scopes);
    const Value size = generateRvalue(*call.arguments[1], emitter, context, scopes);
    const std::string size64 = emitter.freshTemp();
    emitter.line(size64 + " = sext i32 " + size.text + " to i64");
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = call ptr @realloc(ptr " + pointer.text + ", i64 " + size64 + ")");
    emitNullPointerCheck(result, emitter, context);
    return Value{result, Type::rawPtr()};
  }

  LLVMGenerator::Value
  LLVMGenerator::emitRtReleaseBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                            FunctionCodegenContext& context,
                                            const std::vector<Scope>& scopes) const {
    const Value pointer = generateRvalue(*call.arguments[0], emitter, context, scopes);
    emitter.line("call void @free(ptr " + pointer.text + ")");
    return Value{"", Type::voidType()};
  }

  LLVMGenerator::Value
  LLVMGenerator::emitRtSizeofBuiltin(const ast::CallExpression&, IREmitter& emitter,
                                           FunctionCodegenContext& context,
                                           const std::vector<Scope>&) const {
    const Type witness =
        resolveWitnessType(context.functionSpecializationTypeArgs, context.currentFunctionName);
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = add i32 0, " + std::to_string(elementSizeInBytes(witness)));
    return Value{result, Type::i32()};
  }

  LLVMGenerator::Value
  LLVMGenerator::emitRtLoadBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                         FunctionCodegenContext& context,
                                         const std::vector<Scope>& scopes) const {
    const Type witness =
        resolveWitnessType(context.functionSpecializationTypeArgs, context.currentFunctionName);
    const Value pointer = generateRvalue(*call.arguments[0], emitter, context, scopes);
    const Value index = generateRvalue(*call.arguments[1], emitter, context, scopes);
    const std::string elementPointer =
        emitRawBufferElementPointer(pointer, index, witness, emitter);
    const std::string loaded = emitBufferLoad(witness, elementPointer, emitter);
    return Value{loaded, witness};
  }

  LLVMGenerator::Value
  LLVMGenerator::emitRtStoreBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                          FunctionCodegenContext& context,
                                          const std::vector<Scope>& scopes) const {
    const Type witness =
        resolveWitnessType(context.functionSpecializationTypeArgs, context.currentFunctionName);
    const Value pointer = generateRvalue(*call.arguments[0], emitter, context, scopes);
    const Value index = generateRvalue(*call.arguments[1], emitter, context, scopes);
    const Value value = generateRvalue(*call.arguments[2], emitter, context, scopes);
    const std::string elementPointer =
        emitRawBufferElementPointer(pointer, index, witness, emitter);
    emitBufferStore(witness, value.text, elementPointer, emitter);
    return Value{"", Type::voidType()};
  }

  LLVMGenerator::Value
  LLVMGenerator::emitRtLoadPtrBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                            FunctionCodegenContext& context,
                                            const std::vector<Scope>& scopes) const {
    const Value pointer = generateRvalue(*call.arguments[0], emitter, context, scopes);
    const Value index = generateRvalue(*call.arguments[1], emitter, context, scopes);
    const std::string elementPointer =
        emitRawBufferElementPointer(pointer, index, Type::rawPtr(), emitter);
    const std::string loaded = emitter.freshTemp();
    emitter.line(loaded + " = load ptr, ptr " + elementPointer);
    return Value{loaded, Type::rawPtr()};
  }

  LLVMGenerator::Value
  LLVMGenerator::emitRtStorePtrBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                             FunctionCodegenContext& context,
                                             const std::vector<Scope>& scopes) const {
    const Value pointer = generateRvalue(*call.arguments[0], emitter, context, scopes);
    const Value index = generateRvalue(*call.arguments[1], emitter, context, scopes);
    const Value value = generateRvalue(*call.arguments[2], emitter, context, scopes);
    const std::string elementPointer =
        emitRawBufferElementPointer(pointer, index, Type::rawPtr(), emitter);
    emitter.line("store ptr " + value.text + ", ptr " + elementPointer);
    return Value{"", Type::voidType()};
  }

  LLVMGenerator::Value
  LLVMGenerator::emitRtLoadI32Builtin(const ast::CallExpression& call, IREmitter& emitter,
                                            FunctionCodegenContext& context,
                                            const std::vector<Scope>& scopes) const {
    const Value pointer = generateRvalue(*call.arguments[0], emitter, context, scopes);
    const Value index = generateRvalue(*call.arguments[1], emitter, context, scopes);
    const std::string elementPointer =
        emitRawBufferElementPointer(pointer, index, Type::i32(), emitter);
    const std::string loaded = emitter.freshTemp();
    emitter.line(loaded + " = load i32, ptr " + elementPointer);
    return Value{loaded, Type::i32()};
  }

  LLVMGenerator::Value
  LLVMGenerator::emitRtStoreI32Builtin(const ast::CallExpression& call, IREmitter& emitter,
                                             FunctionCodegenContext& context,
                                             const std::vector<Scope>& scopes) const {
    const Value pointer = generateRvalue(*call.arguments[0], emitter, context, scopes);
    const Value index = generateRvalue(*call.arguments[1], emitter, context, scopes);
    const Value value = generateRvalue(*call.arguments[2], emitter, context, scopes);
    const std::string elementPointer =
        emitRawBufferElementPointer(pointer, index, Type::i32(), emitter);
    emitter.line("store i32 " + value.text + ", ptr " + elementPointer);
    return Value{"", Type::voidType()};
  }

  LLVMGenerator::Value
  LLVMGenerator::emitRtTrapBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                         FunctionCodegenContext& context,
                                         const std::vector<Scope>& scopes) const {
    const Value message = generateRvalue(*call.arguments[0], emitter, context, scopes);
    emitter.line("call void @\"__noria.rt.trap\"(ptr " + message.text + ")");
    return Value{"", Type::voidType()};
  }

  LLVMGenerator::Value
  LLVMGenerator::emitRtNullBuiltin(const ast::CallExpression&, IREmitter& emitter,
                                         FunctionCodegenContext&, const std::vector<Scope>&) const {
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = inttoptr i64 0 to ptr");
    return Value{result, Type::rawPtr()};
  }

  LLVMGenerator::Value
  LLVMGenerator::emitRtPtrEqBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                          FunctionCodegenContext& context,
                                          const std::vector<Scope>& scopes) const {
    const Value left = generateRvalue(*call.arguments[0], emitter, context, scopes);
    const Value right = generateRvalue(*call.arguments[1], emitter, context, scopes);
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = icmp eq ptr " + left.text + ", " + right.text);
    return Value{result, Type::boolean()};
  }

  LLVMGenerator::Value
  LLVMGenerator::emitRtHashBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                         FunctionCodegenContext& context,
                                         const std::vector<Scope>& scopes) const {
    const Type witness =
        resolveWitnessType(context.functionSpecializationTypeArgs, context.currentFunctionName);
    const Value key = generateRvalue(*call.arguments[0], emitter, context, scopes);
    const std::string result = emitter.freshTemp();
    if (witness == Type::i32()) {
      const std::string mixed = emitter.freshTemp();
      emitter.line(mixed + " = mul i32 " + key.text + ", 2654435761");
      const std::string masked = emitter.freshTemp();
      emitter.line(masked + " = and i32 " + mixed + ", 2147483647");
      return Value{masked, Type::i32()};
    }
    if (witness == Type::boolean()) {
      emitter.line(result + " = zext i1 " + key.text + " to i32");
      return Value{result, Type::i32()};
    }
    if (witness == Type::str()) {
      emitter.line(result + " = call i32 @noria_hash_str(ptr " + key.text + ")");
      return Value{result, Type::i32()};
    }
    throw CompileError("codegen: __rt_hash unsupported witness type " + witness.name());
  }

  LLVMGenerator::Value
  LLVMGenerator::emitRtByteOffsetBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                               FunctionCodegenContext& context,
                                               const std::vector<Scope>& scopes) const {
    const Value pointer = generateRvalue(*call.arguments[0], emitter, context, scopes);
    const Value bytes = generateRvalue(*call.arguments[1], emitter, context, scopes);
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = getelementptr i8, ptr " + pointer.text + ", i32 " + bytes.text);
    return Value{result, Type::rawPtr()};
  }

} // namespace noria
