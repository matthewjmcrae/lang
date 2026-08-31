#include "CodegenInternal.hpp"

#include "CodegenSupport.hpp"

#include "noria/Builtins.hpp"
#include "noria/Diagnostic.hpp"

#include <array>
#include <optional>
#include <string>
#include <utility>

namespace noria::codegen_detail {

  std::optional<Value>
  BuiltinEmitter::tryGenerateBuiltinCall(const ast::CallExpression& call, IREmitter& emitter,
                                         FunctionCodegenContext& context,
                                         const ExpressionEmitter& expressions) const {
    const BuiltinSignature* descriptor = lookupBuiltin(call.callee);
    if (descriptor == nullptr) {
      return std::nullopt;
    }

    if (auto builtinEmitter = builtinEmitterFor(descriptor->id)) {
      return (this->*(*builtinEmitter))(call, emitter, context, expressions);
    }

    return std::nullopt;
  }

  std::optional<BuiltinEmitter::BuiltinFn> BuiltinEmitter::builtinEmitterFor(BuiltinId id) const {
    static constexpr std::array<std::pair<BuiltinId, BuiltinFn>, 24> emitters{{
        {BuiltinId::Println, &BuiltinEmitter::emitPrintlnBuiltin},
        {BuiltinId::Print, &BuiltinEmitter::emitPrintBuiltin},
        {BuiltinId::PrintInt, &BuiltinEmitter::emitPrintIntBuiltin},
        {BuiltinId::PrintFloat, &BuiltinEmitter::emitPrintFloatBuiltin},
        {BuiltinId::PrintChar, &BuiltinEmitter::emitPrintCharBuiltin},
        {BuiltinId::Sqrt, &BuiltinEmitter::emitSqrtBuiltin},
        {BuiltinId::Pow, &BuiltinEmitter::emitPowBuiltin},
        {BuiltinId::Len, &BuiltinEmitter::emitLenBuiltin},
        {BuiltinId::RtAlloc, &BuiltinEmitter::emitRtAllocBuiltin},
        {BuiltinId::RtRealloc, &BuiltinEmitter::emitRtReallocBuiltin},
        {BuiltinId::RtRelease, &BuiltinEmitter::emitRtReleaseBuiltin},
        {BuiltinId::RtSizeof, &BuiltinEmitter::emitRtSizeofBuiltin},
        {BuiltinId::RtLoad, &BuiltinEmitter::emitRtLoadBuiltin},
        {BuiltinId::RtStore, &BuiltinEmitter::emitRtStoreBuiltin},
        {BuiltinId::RtDrop, &BuiltinEmitter::emitRtDropBuiltin},
        {BuiltinId::RtLoadPtr, &BuiltinEmitter::emitRtLoadPtrBuiltin},
        {BuiltinId::RtStorePtr, &BuiltinEmitter::emitRtStorePtrBuiltin},
        {BuiltinId::RtLoadI32, &BuiltinEmitter::emitRtLoadI32Builtin},
        {BuiltinId::RtStoreI32, &BuiltinEmitter::emitRtStoreI32Builtin},
        {BuiltinId::RtTrap, &BuiltinEmitter::emitRtTrapBuiltin},
        {BuiltinId::RtNull, &BuiltinEmitter::emitRtNullBuiltin},
        {BuiltinId::RtPtrEq, &BuiltinEmitter::emitRtPtrEqBuiltin},
        {BuiltinId::RtHash, &BuiltinEmitter::emitRtHashBuiltin},
        {BuiltinId::RtByteOffset, &BuiltinEmitter::emitRtByteOffsetBuiltin},
    }};

    for (const auto& [candidate, emitter] : emitters) {
      if (candidate == id) {
        return emitter;
      }
    }
    return std::nullopt;
  }

  Value BuiltinEmitter::emitPrintlnBuiltin(const ast::CallExpression&, IREmitter& emitter,
                                           FunctionCodegenContext&,
                                           const ExpressionEmitter&) const {
    emitter.line("call i32 @putchar(i32 10)");
    return Value{"", Type::voidType()};
  }

  Value BuiltinEmitter::emitPrintBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                         FunctionCodegenContext& context,
                                         const ExpressionEmitter& expressions) const {
    const Value argument = expressions.generateRvalue(
        *call.arguments[0], emitter, context, std::nullopt, LLVMGenerator::OwnershipMode::Borrow);
    const std::string formatPointer = emitter.freshTemp();
    emitter.line(formatPointer + " = getelementptr inbounds [3 x i8], ptr @.fmt.str, i32 0, i32 0");
    emitter.line("call i32 (ptr, ...) @printf(ptr " + formatPointer + ", ptr " + argument.text +
                 ")");
    ownership_.emitReleaseIfOwned(argument, emitter, context);
    return Value{"", Type::voidType()};
  }

  Value BuiltinEmitter::emitPrintIntBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                            FunctionCodegenContext& context,
                                            const ExpressionEmitter& expressions) const {
    const Value argument = expressions.generateRvalue(*call.arguments[0], emitter, context);
    emitter.line("call void @noria_print_int(i32 " + argument.text + ")");
    return Value{"", Type::voidType()};
  }

  Value BuiltinEmitter::emitPrintFloatBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                              FunctionCodegenContext& context,
                                              const ExpressionEmitter& expressions) const {
    const Value argument = expressions.generateRvalue(*call.arguments[0], emitter, context);
    const std::string formatPointer = emitter.freshTemp();
    emitter.line(formatPointer +
                 " = getelementptr inbounds [3 x i8], ptr @.fmt.float, i32 0, i32 0");
    emitter.line("call i32 (ptr, ...) @printf(ptr " + formatPointer + ", double " + argument.text +
                 ")");
    return Value{"", Type::voidType()};
  }

  Value BuiltinEmitter::emitPrintCharBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                             FunctionCodegenContext& context,
                                             const ExpressionEmitter& expressions) const {
    const Value argument = expressions.generateRvalue(*call.arguments[0], emitter, context);
    emitter.line("call i32 @putchar(i32 " + argument.text + ")");
    return Value{"", Type::voidType()};
  }

  Value BuiltinEmitter::emitSqrtBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                        FunctionCodegenContext& context,
                                        const ExpressionEmitter& expressions) const {
    const Value argument = expressions.generateRvalue(*call.arguments[0], emitter, context);
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = call double @llvm.sqrt.f64(double " + argument.text + ")");
    return Value{result, Type::f64()};
  }

  Value BuiltinEmitter::emitPowBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                       FunctionCodegenContext& context,
                                       const ExpressionEmitter& expressions) const {
    const Value base = expressions.generateRvalue(*call.arguments[0], emitter, context);
    const Value exponent = expressions.generateRvalue(*call.arguments[1], emitter, context);
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = call double @llvm.pow.f64(double " + base.text + ", double " +
                 exponent.text + ")");
    return Value{result, Type::f64()};
  }

  Value BuiltinEmitter::emitLenBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                       FunctionCodegenContext& context,
                                       const ExpressionEmitter& expressions) const {
    const Value argument = expressions.generateRvalue(
        *call.arguments[0], emitter, context, std::nullopt, LLVMGenerator::OwnershipMode::Borrow);
    if (argument.type == Type::str()) {
      const std::string length = emitter.freshTemp();
      emitter.line(length + " = call i64 @strlen(ptr " + argument.text + ")");
      const std::string result = emitter.freshTemp();
      emitter.line(result + " = trunc i64 " + length + " to i32");
      ownership_.emitReleaseIfOwned(argument, emitter, context);
      return Value{result, Type::i32()};
    }

    const std::string length = emitter.freshTemp();
    emitter.line(length + " = load i64, ptr " + argument.text);
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = trunc i64 " + length + " to i32");
    ownership_.emitReleaseIfOwned(argument, emitter, context);
    return Value{result, Type::i32()};
  }

  Value BuiltinEmitter::emitRtAllocBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                           FunctionCodegenContext& context,
                                           const ExpressionEmitter& expressions) const {
    const Value size = expressions.generateRvalue(*call.arguments[0], emitter, context);
    const std::string size64 = emitter.freshTemp();
    emitter.line(size64 + " = sext i32 " + size.text + " to i64");
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = call ptr @malloc(i64 " + size64 + ")");
    memory_.emitNullPointerCheck(result, emitter, context);
    return Value{result, Type::rawPtr()};
  }

  Value BuiltinEmitter::emitRtReallocBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                             FunctionCodegenContext& context,
                                             const ExpressionEmitter& expressions) const {
    const Value pointer = expressions.generateRvalue(*call.arguments[0], emitter, context);
    const Value size = expressions.generateRvalue(*call.arguments[1], emitter, context);
    const std::string size64 = emitter.freshTemp();
    emitter.line(size64 + " = sext i32 " + size.text + " to i64");
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = call ptr @realloc(ptr " + pointer.text + ", i64 " + size64 + ")");
    memory_.emitNullPointerCheck(result, emitter, context);
    return Value{result, Type::rawPtr()};
  }

  Value BuiltinEmitter::emitRtReleaseBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                             FunctionCodegenContext& context,
                                             const ExpressionEmitter& expressions) const {
    const Value pointer = expressions.generateRvalue(*call.arguments[0], emitter, context);
    emitter.line("call void @free(ptr " + pointer.text + ")");
    return Value{"", Type::voidType()};
  }

  Value BuiltinEmitter::emitRtSizeofBuiltin(const ast::CallExpression&, IREmitter& emitter,
                                            FunctionCodegenContext& context,
                                            const ExpressionEmitter&) const {
    const Type witness = resolveWitnessType(context.module.functionSpecializationTypeArgs,
                                            context.currentFunctionName);
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = add i32 0, " + std::to_string(elementSizeInBytes(witness)));
    return Value{result, Type::i32()};
  }

  Value BuiltinEmitter::emitRtLoadBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                          FunctionCodegenContext& context,
                                          const ExpressionEmitter& expressions) const {
    const Type witness = resolveWitnessType(context.module.functionSpecializationTypeArgs,
                                            context.currentFunctionName);
    const Value pointer = expressions.generateRvalue(*call.arguments[0], emitter, context);
    const Value index = expressions.generateRvalue(*call.arguments[1], emitter, context);
    const std::string elementPointer =
        memory_.emitRawBufferElementPointer(pointer, index, witness, emitter);
    const std::string loaded = memory_.emitBufferLoad(witness, elementPointer, emitter);
    Value result{loaded, witness, false};
    if (witness == Type::str()) {
      result = ownership_.emitCloneValue(result, emitter, context);
    }
    return result;
  }

  Value BuiltinEmitter::emitRtStoreBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                           FunctionCodegenContext& context,
                                           const ExpressionEmitter& expressions) const {
    const Type witness = resolveWitnessType(context.module.functionSpecializationTypeArgs,
                                            context.currentFunctionName);
    const Value pointer = expressions.generateRvalue(*call.arguments[0], emitter, context);
    const Value index = expressions.generateRvalue(*call.arguments[1], emitter, context);
    Value value = expressions.generateRvalue(*call.arguments[2], emitter, context);
    const std::string elementPointer =
        memory_.emitRawBufferElementPointer(pointer, index, witness, emitter);
    Value stored = value;
    if (witness == Type::str()) {
      stored = ownership_.emitCloneValue(value, emitter, context);
    }
    memory_.emitBufferStore(witness, stored.text, elementPointer, emitter);
    ownership_.emitReleaseIfOwned(value, emitter, context);
    return Value{"", Type::voidType()};
  }

  Value BuiltinEmitter::emitRtDropBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                          FunctionCodegenContext& context,
                                          const ExpressionEmitter& expressions) const {
    const Type witness = resolveWitnessType(context.module.functionSpecializationTypeArgs,
                                            context.currentFunctionName);
    const Value pointer = expressions.generateRvalue(*call.arguments[0], emitter, context);
    const Value index = expressions.generateRvalue(*call.arguments[1], emitter, context);
    const std::string elementPointer =
        memory_.emitRawBufferElementPointer(pointer, index, witness, emitter);
    if (witness == Type::str()) {
      const std::string loaded = memory_.emitBufferLoad(witness, elementPointer, emitter);
      ownership_.emitDropValue(Value{loaded, witness, true}, emitter, context);
    }
    return Value{"", Type::voidType()};
  }

  Value BuiltinEmitter::emitRtLoadPtrBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                             FunctionCodegenContext& context,
                                             const ExpressionEmitter& expressions) const {
    const Value pointer = expressions.generateRvalue(*call.arguments[0], emitter, context);
    const Value index = expressions.generateRvalue(*call.arguments[1], emitter, context);
    const std::string elementPointer =
        memory_.emitRawBufferElementPointer(pointer, index, Type::rawPtr(), emitter);
    const std::string loaded = emitter.freshTemp();
    emitter.line(loaded + " = load ptr, ptr " + elementPointer);
    return Value{loaded, Type::rawPtr()};
  }

  Value BuiltinEmitter::emitRtStorePtrBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                              FunctionCodegenContext& context,
                                              const ExpressionEmitter& expressions) const {
    const Value pointer = expressions.generateRvalue(*call.arguments[0], emitter, context);
    const Value index = expressions.generateRvalue(*call.arguments[1], emitter, context);
    const Value value = expressions.generateRvalue(*call.arguments[2], emitter, context);
    const std::string elementPointer =
        memory_.emitRawBufferElementPointer(pointer, index, Type::rawPtr(), emitter);
    emitter.line("store ptr " + value.text + ", ptr " + elementPointer);
    return Value{"", Type::voidType()};
  }

  Value BuiltinEmitter::emitRtLoadI32Builtin(const ast::CallExpression& call, IREmitter& emitter,
                                             FunctionCodegenContext& context,
                                             const ExpressionEmitter& expressions) const {
    const Value pointer = expressions.generateRvalue(*call.arguments[0], emitter, context);
    const Value index = expressions.generateRvalue(*call.arguments[1], emitter, context);
    const std::string elementPointer =
        memory_.emitRawBufferElementPointer(pointer, index, Type::i32(), emitter);
    const std::string loaded = emitter.freshTemp();
    emitter.line(loaded + " = load i32, ptr " + elementPointer);
    return Value{loaded, Type::i32()};
  }

  Value BuiltinEmitter::emitRtStoreI32Builtin(const ast::CallExpression& call, IREmitter& emitter,
                                              FunctionCodegenContext& context,
                                              const ExpressionEmitter& expressions) const {
    const Value pointer = expressions.generateRvalue(*call.arguments[0], emitter, context);
    const Value index = expressions.generateRvalue(*call.arguments[1], emitter, context);
    const Value value = expressions.generateRvalue(*call.arguments[2], emitter, context);
    const std::string elementPointer =
        memory_.emitRawBufferElementPointer(pointer, index, Type::i32(), emitter);
    emitter.line("store i32 " + value.text + ", ptr " + elementPointer);
    return Value{"", Type::voidType()};
  }

  Value BuiltinEmitter::emitRtTrapBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                          FunctionCodegenContext& context,
                                          const ExpressionEmitter& expressions) const {
    const Value message = expressions.generateRvalue(*call.arguments[0], emitter, context);
    emitter.line("call void @\"__noria.rt.trap\"(ptr " + message.text + ")");
    return Value{"", Type::voidType()};
  }

  Value BuiltinEmitter::emitRtNullBuiltin(const ast::CallExpression&, IREmitter& emitter,
                                          FunctionCodegenContext&, const ExpressionEmitter&) const {
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = inttoptr i64 0 to ptr");
    return Value{result, Type::rawPtr()};
  }

  Value BuiltinEmitter::emitRtPtrEqBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                           FunctionCodegenContext& context,
                                           const ExpressionEmitter& expressions) const {
    const Value left = expressions.generateRvalue(*call.arguments[0], emitter, context);
    const Value right = expressions.generateRvalue(*call.arguments[1], emitter, context);
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = icmp eq ptr " + left.text + ", " + right.text);
    return Value{result, Type::boolean()};
  }

  Value BuiltinEmitter::emitRtHashBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                          FunctionCodegenContext& context,
                                          const ExpressionEmitter& expressions) const {
    const Type witness = resolveWitnessType(context.module.functionSpecializationTypeArgs,
                                            context.currentFunctionName);
    const Value key = expressions.generateRvalue(*call.arguments[0], emitter, context, std::nullopt,
                                                 LLVMGenerator::OwnershipMode::Borrow);
    const std::string result = emitter.freshTemp();
    if (witness == Type::i32()) {
      const std::string mixed = emitter.freshTemp();
      emitter.line(mixed + " = mul i32 " + key.text + ", 2654435761");
      const std::string masked = emitter.freshTemp();
      emitter.line(masked + " = and i32 " + mixed + ", 2147483647");
      ownership_.emitReleaseIfOwned(key, emitter, context);
      return Value{masked, Type::i32()};
    }
    if (witness == Type::boolean()) {
      emitter.line(result + " = zext i1 " + key.text + " to i32");
      ownership_.emitReleaseIfOwned(key, emitter, context);
      return Value{result, Type::i32()};
    }
    if (witness == Type::str()) {
      emitter.line(result + " = call i32 @noria_hash_str(ptr " + key.text + ")");
      ownership_.emitReleaseIfOwned(key, emitter, context);
      return Value{result, Type::i32()};
    }
    throw CompileError("codegen: __rt_hash unsupported witness type " + witness.name());
  }

  Value BuiltinEmitter::emitRtByteOffsetBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                                FunctionCodegenContext& context,
                                                const ExpressionEmitter& expressions) const {
    const Value pointer = expressions.generateRvalue(*call.arguments[0], emitter, context);
    const Value bytes = expressions.generateRvalue(*call.arguments[1], emitter, context);
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = getelementptr i8, ptr " + pointer.text + ", i32 " + bytes.text);
    return Value{result, Type::rawPtr()};
  }

} // namespace noria::codegen_detail
