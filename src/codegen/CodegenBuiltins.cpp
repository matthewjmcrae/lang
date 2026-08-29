#include "CodegenState.hpp"

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
  LLVMGenerator::BuiltinsState::tryGenerateBuiltinCall(const ast::CallExpression& call, IREmitter& emitter,
                                              FunctionCodegenContext& context,
                                              const std::vector<Scope>& scopes) const {

    const BuiltinSignature* descriptor = lookupBuiltin(call.callee);
    if (descriptor == nullptr)
      return std::nullopt;

    if (auto builtinEmitter = builtinEmitterFor(descriptor->id)) {
      return (this->*(*builtinEmitter))(call, emitter, context, scopes);
    }

    return std::nullopt;
  }

  std::optional<LLVMGenerator::BuiltinsState::BuiltinEmitter>
  LLVMGenerator::BuiltinsState::builtinEmitterFor(BuiltinId id) const {
    static constexpr std::array<std::pair<BuiltinId, BuiltinEmitter>, 24> emitters{{
        {BuiltinId::Println, &BuiltinsState::emitPrintlnBuiltin},
        {BuiltinId::Print, &BuiltinsState::emitPrintBuiltin},
        {BuiltinId::PrintInt, &BuiltinsState::emitPrintIntBuiltin},
        {BuiltinId::PrintFloat, &BuiltinsState::emitPrintFloatBuiltin},
        {BuiltinId::PrintChar, &BuiltinsState::emitPrintCharBuiltin},
        {BuiltinId::Sqrt, &BuiltinsState::emitSqrtBuiltin},
        {BuiltinId::Pow, &BuiltinsState::emitPowBuiltin},
        {BuiltinId::Len, &BuiltinsState::emitLenBuiltin},
        {BuiltinId::RtAlloc, &BuiltinsState::emitRtAllocBuiltin},
        {BuiltinId::RtRealloc, &BuiltinsState::emitRtReallocBuiltin},
        {BuiltinId::RtRelease, &BuiltinsState::emitRtReleaseBuiltin},
        {BuiltinId::RtSizeof, &BuiltinsState::emitRtSizeofBuiltin},
        {BuiltinId::RtLoad, &BuiltinsState::emitRtLoadBuiltin},
        {BuiltinId::RtStore, &BuiltinsState::emitRtStoreBuiltin},
        {BuiltinId::RtDrop, &BuiltinsState::emitRtDropBuiltin},
        {BuiltinId::RtLoadPtr, &BuiltinsState::emitRtLoadPtrBuiltin},
        {BuiltinId::RtStorePtr, &BuiltinsState::emitRtStorePtrBuiltin},
        {BuiltinId::RtLoadI32, &BuiltinsState::emitRtLoadI32Builtin},
        {BuiltinId::RtStoreI32, &BuiltinsState::emitRtStoreI32Builtin},
        {BuiltinId::RtTrap, &BuiltinsState::emitRtTrapBuiltin},
        {BuiltinId::RtNull, &BuiltinsState::emitRtNullBuiltin},
        {BuiltinId::RtPtrEq, &BuiltinsState::emitRtPtrEqBuiltin},
        {BuiltinId::RtHash, &BuiltinsState::emitRtHashBuiltin},
        {BuiltinId::RtByteOffset, &BuiltinsState::emitRtByteOffsetBuiltin},
    }};

    for (const auto& [candidate, emitter] : emitters) {
      if (candidate == id) {
        return emitter;
      }
    }
    return std::nullopt;
  }

  LLVMGenerator::Value
  LLVMGenerator::BuiltinsState::emitPrintlnBuiltin(const ast::CallExpression&, IREmitter& emitter,
                                          FunctionCodegenContext&,
                                          const std::vector<Scope>&) const {
    emitter.line("call i32 @putchar(i32 10)");
    return Value{"", Type::voidType()};
  }

  LLVMGenerator::Value
  LLVMGenerator::BuiltinsState::emitPrintBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                        FunctionCodegenContext& context,
                                        const std::vector<Scope>& scopes) const {
    const Value argument = generator().generateRvalue(*call.arguments[0], emitter, context, scopes);
    const std::string formatPointer = emitter.freshTemp();
    emitter.line(formatPointer +
                 " = getelementptr inbounds [3 x i8], ptr @.fmt.str, i32 0, i32 0");
    emitter.line("call i32 (ptr, ...) @printf(ptr " + formatPointer + ", ptr " + argument.text +
                 ")");
    return Value{"", Type::voidType()};
  }

  LLVMGenerator::Value
  LLVMGenerator::BuiltinsState::emitPrintIntBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                           FunctionCodegenContext& context,
                                           const std::vector<Scope>& scopes) const {
    const Value argument = generator().generateRvalue(*call.arguments[0], emitter, context, scopes);
    emitter.line("call void @noria_print_int(i32 " + argument.text + ")");
    return Value{"", Type::voidType()};
  }

  LLVMGenerator::Value
  LLVMGenerator::BuiltinsState::emitPrintFloatBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                             FunctionCodegenContext& context,
                                             const std::vector<Scope>& scopes) const {
    const Value argument = generator().generateRvalue(*call.arguments[0], emitter, context, scopes);
    const std::string formatPointer = emitter.freshTemp();
    emitter.line(formatPointer +
                 " = getelementptr inbounds [3 x i8], ptr @.fmt.float, i32 0, i32 0");
    emitter.line("call i32 (ptr, ...) @printf(ptr " + formatPointer + ", double " + argument.text +
                 ")");
    return Value{"", Type::voidType()};
  }

  LLVMGenerator::Value
  LLVMGenerator::BuiltinsState::emitPrintCharBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                            FunctionCodegenContext& context,
                                            const std::vector<Scope>& scopes) const {
    const Value argument = generator().generateRvalue(*call.arguments[0], emitter, context, scopes);
    emitter.line("call i32 @putchar(i32 " + argument.text + ")");
    return Value{"", Type::voidType()};
  }

  LLVMGenerator::Value
  LLVMGenerator::BuiltinsState::emitSqrtBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                       FunctionCodegenContext& context,
                                       const std::vector<Scope>& scopes) const {
    const Value argument = generator().generateRvalue(*call.arguments[0], emitter, context, scopes);
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = call double @llvm.sqrt.f64(double " + argument.text + ")");
    return Value{result, Type::f64()};
  }

  LLVMGenerator::Value
  LLVMGenerator::BuiltinsState::emitPowBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                      FunctionCodegenContext& context,
                                      const std::vector<Scope>& scopes) const {
    const Value base = generator().generateRvalue(*call.arguments[0], emitter, context, scopes);
    const Value exponent = generator().generateRvalue(*call.arguments[1], emitter, context, scopes);
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = call double @llvm.pow.f64(double " + base.text + ", double " +
                 exponent.text + ")");
    return Value{result, Type::f64()};
  }

  LLVMGenerator::Value
  LLVMGenerator::BuiltinsState::emitLenBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                      FunctionCodegenContext& context,
                                      const std::vector<Scope>& scopes) const {
    const Value argument =
        generator().generateRvalue(*call.arguments[0], emitter, context, scopes, std::nullopt,
                                   LLVMGenerator::OwnershipMode::Borrow);
    if (argument.type == Type::str()) {
      const std::string length = emitter.freshTemp();
      emitter.line(length + " = call i64 @strlen(ptr " + argument.text + ")");
      const std::string result = emitter.freshTemp();
      emitter.line(result + " = trunc i64 " + length + " to i32");
      generator().emitReleaseIfOwned(argument, emitter, context);
      return Value{result, Type::i32()};
    }

    const std::string length = emitter.freshTemp();
    emitter.line(length + " = load i64, ptr " + argument.text);
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = trunc i64 " + length + " to i32");
    generator().emitReleaseIfOwned(argument, emitter, context);
    return Value{result, Type::i32()};
  }

  LLVMGenerator::Value
  LLVMGenerator::BuiltinsState::emitRtAllocBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                          FunctionCodegenContext& context,
                                          const std::vector<Scope>& scopes) const {
    const Value size = generator().generateRvalue(*call.arguments[0], emitter, context, scopes);
    const std::string size64 = emitter.freshTemp();
    emitter.line(size64 + " = sext i32 " + size.text + " to i64");
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = call ptr @malloc(i64 " + size64 + ")");
    generator().emitNullPointerCheck(result, emitter, context);
    return Value{result, Type::rawPtr()};
  }

  LLVMGenerator::Value
  LLVMGenerator::BuiltinsState::emitRtReallocBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                            FunctionCodegenContext& context,
                                            const std::vector<Scope>& scopes) const {
    const Value pointer = generator().generateRvalue(*call.arguments[0], emitter, context, scopes);
    const Value size = generator().generateRvalue(*call.arguments[1], emitter, context, scopes);
    const std::string size64 = emitter.freshTemp();
    emitter.line(size64 + " = sext i32 " + size.text + " to i64");
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = call ptr @realloc(ptr " + pointer.text + ", i64 " + size64 + ")");
    generator().emitNullPointerCheck(result, emitter, context);
    return Value{result, Type::rawPtr()};
  }

  LLVMGenerator::Value
  LLVMGenerator::BuiltinsState::emitRtReleaseBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                            FunctionCodegenContext& context,
                                            const std::vector<Scope>& scopes) const {
    const Value pointer = generator().generateRvalue(*call.arguments[0], emitter, context, scopes);
    emitter.line("call void @free(ptr " + pointer.text + ")");
    return Value{"", Type::voidType()};
  }

  LLVMGenerator::Value
  LLVMGenerator::BuiltinsState::emitRtSizeofBuiltin(const ast::CallExpression&, IREmitter& emitter,
                                           FunctionCodegenContext& context,
                                           const std::vector<Scope>&) const {
    const Type witness =
        resolveWitnessType(context.functionSpecializationTypeArgs, context.currentFunctionName);
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = add i32 0, " + std::to_string(elementSizeInBytes(witness)));
    return Value{result, Type::i32()};
  }

  LLVMGenerator::Value
  LLVMGenerator::BuiltinsState::emitRtLoadBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                         FunctionCodegenContext& context,
                                         const std::vector<Scope>& scopes) const {
    const Type witness =
        resolveWitnessType(context.functionSpecializationTypeArgs, context.currentFunctionName);
    const Value pointer = generator().generateRvalue(*call.arguments[0], emitter, context, scopes);
    const Value index = generator().generateRvalue(*call.arguments[1], emitter, context, scopes);
    const std::string elementPointer =
        generator().emitRawBufferElementPointer(pointer, index, witness, emitter);
    const std::string loaded = generator().emitBufferLoad(witness, elementPointer, emitter);
    Value result{loaded, witness, false};
    if (witness == Type::str()) {
      result = generator().emitCloneValue(result, emitter, context);
    }
    return result;
  }

  LLVMGenerator::Value
  LLVMGenerator::BuiltinsState::emitRtStoreBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                          FunctionCodegenContext& context,
                                          const std::vector<Scope>& scopes) const {
    const Type witness =
        resolveWitnessType(context.functionSpecializationTypeArgs, context.currentFunctionName);
    const Value pointer = generator().generateRvalue(*call.arguments[0], emitter, context, scopes);
    const Value index = generator().generateRvalue(*call.arguments[1], emitter, context, scopes);
    Value value = generator().generateRvalue(*call.arguments[2], emitter, context, scopes);
    const std::string elementPointer =
        generator().emitRawBufferElementPointer(pointer, index, witness, emitter);
    Value stored = value;
    if (witness == Type::str()) {
      stored = generator().emitCloneValue(value, emitter, context);
    }
    generator().emitBufferStore(witness, stored.text, elementPointer, emitter);
    generator().emitReleaseIfOwned(value, emitter, context);
    return Value{"", Type::voidType()};
  }

  LLVMGenerator::Value
  LLVMGenerator::BuiltinsState::emitRtDropBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                         FunctionCodegenContext& context,
                                         const std::vector<Scope>& scopes) const {
    const Type witness =
        resolveWitnessType(context.functionSpecializationTypeArgs, context.currentFunctionName);
    const Value pointer = generator().generateRvalue(*call.arguments[0], emitter, context, scopes);
    const Value index = generator().generateRvalue(*call.arguments[1], emitter, context, scopes);
    const std::string elementPointer =
        generator().emitRawBufferElementPointer(pointer, index, witness, emitter);
    if (witness == Type::str()) {
      const std::string loaded = generator().emitBufferLoad(witness, elementPointer, emitter);
      generator().emitDropValue(Value{loaded, witness, true}, emitter, context);
    }
    return Value{"", Type::voidType()};
  }

  LLVMGenerator::Value
  LLVMGenerator::BuiltinsState::emitRtLoadPtrBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                            FunctionCodegenContext& context,
                                            const std::vector<Scope>& scopes) const {
    const Value pointer = generator().generateRvalue(*call.arguments[0], emitter, context, scopes);
    const Value index = generator().generateRvalue(*call.arguments[1], emitter, context, scopes);
    const std::string elementPointer =
        generator().emitRawBufferElementPointer(pointer, index, Type::rawPtr(), emitter);
    const std::string loaded = emitter.freshTemp();
    emitter.line(loaded + " = load ptr, ptr " + elementPointer);
    return Value{loaded, Type::rawPtr()};
  }

  LLVMGenerator::Value
  LLVMGenerator::BuiltinsState::emitRtStorePtrBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                             FunctionCodegenContext& context,
                                             const std::vector<Scope>& scopes) const {
    const Value pointer = generator().generateRvalue(*call.arguments[0], emitter, context, scopes);
    const Value index = generator().generateRvalue(*call.arguments[1], emitter, context, scopes);
    const Value value = generator().generateRvalue(*call.arguments[2], emitter, context, scopes);
    const std::string elementPointer =
        generator().emitRawBufferElementPointer(pointer, index, Type::rawPtr(), emitter);
    emitter.line("store ptr " + value.text + ", ptr " + elementPointer);
    return Value{"", Type::voidType()};
  }

  LLVMGenerator::Value
  LLVMGenerator::BuiltinsState::emitRtLoadI32Builtin(const ast::CallExpression& call, IREmitter& emitter,
                                            FunctionCodegenContext& context,
                                            const std::vector<Scope>& scopes) const {
    const Value pointer = generator().generateRvalue(*call.arguments[0], emitter, context, scopes);
    const Value index = generator().generateRvalue(*call.arguments[1], emitter, context, scopes);
    const std::string elementPointer =
        generator().emitRawBufferElementPointer(pointer, index, Type::i32(), emitter);
    const std::string loaded = emitter.freshTemp();
    emitter.line(loaded + " = load i32, ptr " + elementPointer);
    return Value{loaded, Type::i32()};
  }

  LLVMGenerator::Value
  LLVMGenerator::BuiltinsState::emitRtStoreI32Builtin(const ast::CallExpression& call, IREmitter& emitter,
                                             FunctionCodegenContext& context,
                                             const std::vector<Scope>& scopes) const {
    const Value pointer = generator().generateRvalue(*call.arguments[0], emitter, context, scopes);
    const Value index = generator().generateRvalue(*call.arguments[1], emitter, context, scopes);
    const Value value = generator().generateRvalue(*call.arguments[2], emitter, context, scopes);
    const std::string elementPointer =
        generator().emitRawBufferElementPointer(pointer, index, Type::i32(), emitter);
    emitter.line("store i32 " + value.text + ", ptr " + elementPointer);
    return Value{"", Type::voidType()};
  }

  LLVMGenerator::Value
  LLVMGenerator::BuiltinsState::emitRtTrapBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                         FunctionCodegenContext& context,
                                         const std::vector<Scope>& scopes) const {
    const Value message = generator().generateRvalue(*call.arguments[0], emitter, context, scopes);
    emitter.line("call void @\"__noria.rt.trap\"(ptr " + message.text + ")");
    return Value{"", Type::voidType()};
  }

  LLVMGenerator::Value
  LLVMGenerator::BuiltinsState::emitRtNullBuiltin(const ast::CallExpression&, IREmitter& emitter,
                                         FunctionCodegenContext&, const std::vector<Scope>&) const {
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = inttoptr i64 0 to ptr");
    return Value{result, Type::rawPtr()};
  }

  LLVMGenerator::Value
  LLVMGenerator::BuiltinsState::emitRtPtrEqBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                          FunctionCodegenContext& context,
                                          const std::vector<Scope>& scopes) const {
    const Value left = generator().generateRvalue(*call.arguments[0], emitter, context, scopes);
    const Value right = generator().generateRvalue(*call.arguments[1], emitter, context, scopes);
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = icmp eq ptr " + left.text + ", " + right.text);
    return Value{result, Type::boolean()};
  }

  LLVMGenerator::Value
  LLVMGenerator::BuiltinsState::emitRtHashBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                         FunctionCodegenContext& context,
                                         const std::vector<Scope>& scopes) const {
    const Type witness =
        resolveWitnessType(context.functionSpecializationTypeArgs, context.currentFunctionName);
    // Borrow: hashing must not clone managed keys (str), or each probe leaks.
    const Value key =
        generator().generateRvalue(*call.arguments[0], emitter, context, scopes, std::nullopt,
                                   LLVMGenerator::OwnershipMode::Borrow);
    const std::string result = emitter.freshTemp();
    if (witness == Type::i32()) {
      const std::string mixed = emitter.freshTemp();
      emitter.line(mixed + " = mul i32 " + key.text + ", 2654435761");
      const std::string masked = emitter.freshTemp();
      emitter.line(masked + " = and i32 " + mixed + ", 2147483647");
      generator().emitReleaseIfOwned(key, emitter, context);
      return Value{masked, Type::i32()};
    }
    if (witness == Type::boolean()) {
      emitter.line(result + " = zext i1 " + key.text + " to i32");
      generator().emitReleaseIfOwned(key, emitter, context);
      return Value{result, Type::i32()};
    }
    if (witness == Type::str()) {
      emitter.line(result + " = call i32 @noria_hash_str(ptr " + key.text + ")");
      generator().emitReleaseIfOwned(key, emitter, context);
      return Value{result, Type::i32()};
    }
    throw CompileError("codegen: __rt_hash unsupported witness type " + witness.name());
  }

  LLVMGenerator::Value
  LLVMGenerator::BuiltinsState::emitRtByteOffsetBuiltin(const ast::CallExpression& call, IREmitter& emitter,
                                               FunctionCodegenContext& context,
                                               const std::vector<Scope>& scopes) const {
    const Value pointer = generator().generateRvalue(*call.arguments[0], emitter, context, scopes);
    const Value bytes = generator().generateRvalue(*call.arguments[1], emitter, context, scopes);
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = getelementptr i8, ptr " + pointer.text + ", i32 " + bytes.text);
    return Value{result, Type::rawPtr()};
  }

} // namespace noria
