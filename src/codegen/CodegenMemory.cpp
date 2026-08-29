#include "CodegenInternal.hpp"

#include "CodegenSupport.hpp"

#include "noria/Types.hpp"

#include <string>
#include <string_view>

namespace noria::codegen_detail {

  std::string MemoryEmitter::emitArrayElementPointer(const Value& base, const Value& indexValue,
                                                     const Type& elementType, IREmitter& emitter,
                                                     FunctionCodegenContext& context) const {
    const std::string length = emitter.freshTemp();
    emitter.line(length + " = load i64, ptr " + base.text);
    emitBoundsCheck(length, indexValue, emitter, context, "array index out of bounds\n");

    const std::string elems = emitter.freshTemp();
    emitter.line(elems + " = getelementptr inbounds i8, ptr " + base.text + ", i64 8");
    return emitRawBufferElementPointer(Value{elems, Type::rawPtr()}, indexValue, elementType,
                                       emitter);
  }

  std::string MemoryEmitter::emitCStringPointer(std::string_view text, IREmitter& emitter,
                                                FunctionCodegenContext& context) const {
    const std::string globalName = "@.str." + std::to_string(context.module.nextStringGlobal++);
    const std::size_t length = text.size() + 1;
    context.module.globals << globalName << " = private unnamed_addr constant { i32, [" << length
                           << " x i8] } { i32 -1, [" << length << " x i8] c\""
                           << escapeForLLVMString(text) << "\\00\" }\n";

    const std::string result = emitter.freshTemp();
    emitter.line(result + " = getelementptr inbounds { i32, [" + std::to_string(length) +
                 " x i8] }, ptr " + globalName + ", i32 0, i32 1");
    return result;
  }

  void MemoryEmitter::emitRuntimeTrap(IREmitter& emitter, FunctionCodegenContext& context,
                                      std::string_view message) const {
    const std::string pointer = emitCStringPointer(message, emitter, context);
    emitter.line("call void @\"__noria.rt.trap\"(ptr " + pointer + ")");
    emitter.line("unreachable");
  }

  void MemoryEmitter::emitTrapUnless(const std::string& condition, std::string_view labelPrefix,
                                     IREmitter& emitter, FunctionCodegenContext& context,
                                     std::string_view message) const {
    const int labelId = emitter.freshLabelId();
    const std::string trapLabel = std::string(labelPrefix) + ".fail" + std::to_string(labelId);
    const std::string contLabel = std::string(labelPrefix) + ".ok" + std::to_string(labelId);
    emitter.emitCondBranch(condition, contLabel, trapLabel);
    emitter.emitLabel(trapLabel);
    emitRuntimeTrap(emitter, context, message);
    emitter.emitLabel(contLabel);
  }

  void MemoryEmitter::emitNullPointerCheck(const std::string& pointer, IREmitter& emitter,
                                           FunctionCodegenContext& context) const {
    const std::string isNonNull = emitter.freshTemp();
    emitter.line(isNonNull + " = icmp ne ptr " + pointer + ", null");
    emitTrapUnless(isNonNull, "alloc", emitter, context, "allocation failed\n");
  }

  std::string MemoryEmitter::emitCheckedMalloc(const std::string& size64, IREmitter& emitter,
                                               FunctionCodegenContext& context) const {
    const std::string pointer = emitter.freshTemp();
    emitter.line(pointer + " = call ptr @malloc(i64 " + size64 + ")");
    emitNullPointerCheck(pointer, emitter, context);
    return pointer;
  }

  void MemoryEmitter::emitBoundsCheck(const std::string& length64, const Value& indexValue,
                                      IREmitter& emitter, FunctionCodegenContext& context,
                                      std::string_view message) const {
    const std::string index64 = emitter.freshTemp();
    emitter.line(index64 + " = zext i32 " + indexValue.text + " to i64");
    const std::string inBounds = emitter.freshTemp();
    emitter.line(inBounds + " = icmp ult i64 " + index64 + ", " + length64);
    emitTrapUnless(inBounds, "bounds", emitter, context, message);
  }

  std::string MemoryEmitter::emitRawBufferElementPointer(const Value& base, const Value& indexValue,
                                                         const Type& elementType,
                                                         IREmitter& emitter) const {
    const std::size_t size = elementSizeInBytes(elementType);
    const std::string offset = emitter.freshTemp();
    emitter.line(offset + " = mul i32 " + indexValue.text + ", " + std::to_string(size));
    const std::string pointer = emitter.freshTemp();
    emitter.line(pointer + " = getelementptr i8, ptr " + base.text + ", i32 " + offset);
    return pointer;
  }

  std::string MemoryEmitter::emitBufferLoad(const Type& type, const std::string& pointer,
                                            IREmitter& emitter) const {
    if (type.kind() == TypeKind::Bool) {
      const std::string packed = emitter.freshTemp();
      emitter.line(packed + " = load i8, ptr " + pointer);
      const std::string result = emitter.freshTemp();
      emitter.line(result + " = icmp ne i8 " + packed + ", 0");
      return result;
    }

    const std::string result = emitter.freshTemp();
    emitter.line(result + " = load " + LLVMType(type) + ", ptr " + pointer);
    return result;
  }

  void MemoryEmitter::emitBufferStore(const Type& type, const std::string& value,
                                      const std::string& pointer, IREmitter& emitter) const {
    if (type.kind() == TypeKind::Bool) {
      const std::string packed = emitter.freshTemp();
      emitter.line(packed + " = zext i1 " + value + " to i8");
      emitter.line("store i8 " + packed + ", ptr " + pointer);
      return;
    }

    emitter.line("store " + LLVMType(type) + " " + value + ", ptr " + pointer);
  }

} // namespace noria::codegen_detail
