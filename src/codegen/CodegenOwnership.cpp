#include "CodegenInternal.hpp"

#include "CodegenSupport.hpp"

#include "noria/SemanticTables.hpp"
#include "noria/Types.hpp"

#include <string>

namespace noria::codegen_detail {

  bool OwnershipEmitter::typeNeedsDrop(const Type& type,
                                       const FunctionCodegenContext& context) const {
    if (type.kind() == TypeKind::Str || type.kind() == TypeKind::Array) {
      return true;
    }
    if (type.kind() != TypeKind::Struct) {
      return false;
    }
    if (standardContainerKindFromStructName(type.structName())) {
      return true;
    }

    const StructLayout& layout = structs_.lookupStructLayout(context, type);
    for (const Type& fieldType : layout.fieldTypes) {
      if (typeContainsManaged(fieldType, context)) {
        return true;
      }
    }
    return false;
  }

  bool OwnershipEmitter::typeContainsManaged(const Type& type,
                                             const FunctionCodegenContext& context) const {
    return typeNeedsDrop(type, context);
  }

  void OwnershipEmitter::emitReleaseIfOwned(const Value& value, IREmitter& emitter,
                                            FunctionCodegenContext& context) const {
    if (value.owned) {
      emitDropValue(value, emitter, context);
    }
  }

  void OwnershipEmitter::emitDropValue(const Value& value, IREmitter& emitter,
                                       FunctionCodegenContext& context) const {
    if (value.type == Type::str()) {
      emitter.line("call void @__noria.rt.drop_str(ptr " + value.text + ")");
      return;
    }

    if (value.type.kind() == TypeKind::Array) {
      const Type elementType = value.type.elementType();
      const std::string length = emitter.freshTemp();
      emitter.line(length + " = load i64, ptr " + value.text);

      if (typeContainsManaged(elementType, context)) {
        const int labelId = emitter.freshLabelId();
        const std::string indexSlot = emitter.freshTemp();
        emitter.line(indexSlot + " = alloca i64");
        emitter.line("store i64 0, ptr " + indexSlot);
        const std::string conditionLabel = "array.drop.cond" + std::to_string(labelId);
        const std::string bodyLabel = "array.drop.body" + std::to_string(labelId);
        const std::string endLabel = "array.drop.end" + std::to_string(labelId);
        emitter.emitBranch(conditionLabel);
        emitter.emitLabel(conditionLabel);
        const std::string index64 = emitter.freshTemp();
        emitter.line(index64 + " = load i64, ptr " + indexSlot);
        const std::string inRange = emitter.freshTemp();
        emitter.line(inRange + " = icmp ult i64 " + index64 + ", " + length);
        emitter.emitCondBranch(inRange, bodyLabel, endLabel);

        emitter.emitLabel(bodyLabel);
        const std::string index = emitter.freshTemp();
        emitter.line(index + " = trunc i64 " + index64 + " to i32");
        const Value indexValue{index, Type::i32()};
        const std::string elems = emitter.freshTemp();
        emitter.line(elems + " = getelementptr inbounds i8, ptr " + value.text + ", i64 8");
        const std::string elementPointer = memory_.emitRawBufferElementPointer(
            Value{elems, Type::rawPtr()}, indexValue, elementType, emitter);
        const std::string elementText =
            memory_.emitBufferLoad(elementType, elementPointer, emitter);
        emitDropValue(Value{elementText, elementType, true}, emitter, context);

        const std::string nextIndex = emitter.freshTemp();
        emitter.line(nextIndex + " = add i64 " + index64 + ", 1");
        emitter.line("store i64 " + nextIndex + ", ptr " + indexSlot);
        emitter.emitBranch(conditionLabel);
        emitter.emitLabel(endLabel);
      }

      emitter.line("call void @free(ptr " + value.text + ")");
      return;
    }

    if (value.type.kind() == TypeKind::Struct) {
      if (const std::optional<StandardContainer> container =
              standardContainerKindFromStructName(value.type.structName())) {
        const std::vector<Type> typeArgs = specializedStructTypeArgs(value.type, context);
        (void)emitStandardContainerCall(*container, ContainerOperation::Drop, typeArgs, {value},
                                        emitter, context);
        return;
      }

      const std::string slot = emitter.freshTemp();
      emitter.emitAlloca(value.type, slot);
      emitter.emitStore(value.type, value.text, slot);
      const StructLayout& layout = structs_.lookupStructLayout(context, value.type);
      for (std::size_t index{}; index < layout.fieldTypes.size(); ++index) {
        const Type& fieldType = layout.fieldTypes[index];
        if (!typeContainsManaged(fieldType, context)) {
          continue;
        }
        const std::string fieldPointer =
            structs_.emitStructFieldPointer(value.type, slot, index, emitter);
        const std::string fieldText = memory_.emitBufferLoad(fieldType, fieldPointer, emitter);
        emitDropValue(Value{fieldText, fieldType, true}, emitter, context);
      }
    }
  }

  Value OwnershipEmitter::emitCloneValue(const Value& value, IREmitter& emitter,
                                         FunctionCodegenContext& context) const {
    if (value.type == Type::str()) {
      const std::string cloned = emitter.freshTemp();
      emitter.line(cloned + " = call ptr @__noria.rt.clone_str(ptr " + value.text + ")");
      return Value{cloned, Type::str(), true};
    }

    if (value.type.kind() == TypeKind::Array) {
      const Type elementType = value.type.elementType();
      const std::string length = emitter.freshTemp();
      emitter.line(length + " = load i64, ptr " + value.text);
      const std::string payloadBytes = emitter.freshTemp();
      emitter.line(payloadBytes + " = mul i64 " + length + ", " +
                   std::to_string(elementSizeInBytes(elementType)));
      const std::string totalBytes = emitter.freshTemp();
      emitter.line(totalBytes + " = add i64 " + payloadBytes + ", 8");
      const std::string cloneBase = memory_.emitCheckedMalloc(totalBytes, emitter, context);
      emitter.line("store i64 " + length + ", ptr " + cloneBase);

      const std::string sourceElements = emitter.freshTemp();
      emitter.line(sourceElements + " = getelementptr inbounds i8, ptr " + value.text + ", i64 8");
      const std::string destElements = emitter.freshTemp();
      emitter.line(destElements + " = getelementptr inbounds i8, ptr " + cloneBase + ", i64 8");

      const int labelId = emitter.freshLabelId();
      const std::string indexSlot = emitter.freshTemp();
      emitter.line(indexSlot + " = alloca i64");
      emitter.line("store i64 0, ptr " + indexSlot);
      const std::string conditionLabel = "array.clone.cond" + std::to_string(labelId);
      const std::string bodyLabel = "array.clone.body" + std::to_string(labelId);
      const std::string endLabel = "array.clone.end" + std::to_string(labelId);
      emitter.emitBranch(conditionLabel);
      emitter.emitLabel(conditionLabel);
      const std::string index64 = emitter.freshTemp();
      emitter.line(index64 + " = load i64, ptr " + indexSlot);
      const std::string inRange = emitter.freshTemp();
      emitter.line(inRange + " = icmp ult i64 " + index64 + ", " + length);
      emitter.emitCondBranch(inRange, bodyLabel, endLabel);

      emitter.emitLabel(bodyLabel);
      const std::string index = emitter.freshTemp();
      emitter.line(index + " = trunc i64 " + index64 + " to i32");
      const Value indexValue{index, Type::i32()};
      const std::string sourcePointer = memory_.emitRawBufferElementPointer(
          Value{sourceElements, Type::rawPtr()}, indexValue, elementType, emitter);
      const std::string sourceText = memory_.emitBufferLoad(elementType, sourcePointer, emitter);
      const Value clonedElement =
          emitCloneValue(Value{sourceText, elementType, true}, emitter, context);
      const std::string destPointer = memory_.emitRawBufferElementPointer(
          Value{destElements, Type::rawPtr()}, indexValue, elementType, emitter);
      memory_.emitBufferStore(elementType, clonedElement.text, destPointer, emitter);

      const std::string nextIndex = emitter.freshTemp();
      emitter.line(nextIndex + " = add i64 " + index64 + ", 1");
      emitter.line("store i64 " + nextIndex + ", ptr " + indexSlot);
      emitter.emitBranch(conditionLabel);
      emitter.emitLabel(endLabel);
      return Value{cloneBase, value.type, true};
    }

    if (value.type.kind() == TypeKind::Struct) {
      if (const std::optional<StandardContainer> container =
              standardContainerKindFromStructName(value.type.structName())) {
        const std::vector<Type> typeArgs = specializedStructTypeArgs(value.type, context);
        Value cloned = emitStandardContainerCall(*container, ContainerOperation::Clone, typeArgs,
                                                 {value}, emitter, context);
        cloned.owned = true;
        return cloned;
      }

      const std::string slot = emitter.freshTemp();
      emitter.emitAlloca(value.type, slot);
      emitter.emitStore(value.type, value.text, slot);
      const StructLayout& layout = structs_.lookupStructLayout(context, value.type);
      for (std::size_t index{}; index < layout.fieldTypes.size(); ++index) {
        const Type& fieldType = layout.fieldTypes[index];
        if (!typeContainsManaged(fieldType, context)) {
          continue;
        }
        const std::string fieldPointer =
            structs_.emitStructFieldPointer(value.type, slot, index, emitter);
        const std::string fieldText = memory_.emitBufferLoad(fieldType, fieldPointer, emitter);
        const Value clonedField =
            emitCloneValue(Value{fieldText, fieldType, true}, emitter, context);
        memory_.emitBufferStore(fieldType, clonedField.text, fieldPointer, emitter);
      }

      const std::string result = emitter.freshTemp();
      emitter.emitLoad(value.type, slot, result);
      return Value{result, value.type, true};
    }

    return value;
  }

  void OwnershipEmitter::emitDropLocal(const LocalBinding& local, IREmitter& emitter,
                                       FunctionCodegenContext& context) const {
    if (local.ownedSlot.empty()) {
      return;
    }

    const std::string owned = emitter.freshTemp();
    emitter.line(owned + " = load i1, ptr " + local.ownedSlot);
    const int labelId = emitter.freshLabelId();
    const std::string bodyLabel = "drop.local.body" + std::to_string(labelId);
    const std::string endLabel = "drop.local.end" + std::to_string(labelId);
    emitter.emitCondBranch(owned, bodyLabel, endLabel);
    emitter.emitLabel(bodyLabel);
    const std::string valueText = emitter.freshTemp();
    if (local.byteBuffer) {
      emitter.line(valueText + " = load " + LLVMType(local.type) + ", ptr " + local.slot);
    } else {
      emitter.emitLoad(local.type, local.slot, valueText);
    }
    emitDropValue(Value{valueText, local.type, true}, emitter, context);
    emitter.emitBranch(endLabel);
    emitter.emitLabel(endLabel);
  }

  void OwnershipEmitter::emitDropScope(Scope& scope, IREmitter& emitter,
                                       FunctionCodegenContext& context) const {
    if (!scope.containsPtr) {
      return;
    }

    for (const auto& [name, binding] : scope.bindings) {
      (void)name;
      emitDropLocal(binding, emitter, context);
    }
  }

  void OwnershipEmitter::emitDropScopes(std::vector<Scope>& scopes, IREmitter& emitter,
                                        FunctionCodegenContext& context) const {
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
      emitDropScope(*scope, emitter, context);
    }
  }

  void OwnershipEmitter::emitStoreManagedLocal(const LocalBinding& local, const Value& value,
                                               IREmitter& emitter,
                                               FunctionCodegenContext& context) const {
    (void)context;
    if (local.byteBuffer) {
      memory_.emitBufferStore(local.type, value.text, local.slot, emitter);
    } else {
      emitter.emitStore(local.type, value.text, local.slot);
    }

    if (!local.ownedSlot.empty()) {
      const std::string ownedValue = value.owned ? "true" : "false";
      emitter.line("store i1 " + ownedValue + ", ptr " + local.ownedSlot);
    }
  }

  void OwnershipEmitter::emitAssignPlace(const LocalBinding& dest, Value rvalue, IREmitter& emitter,
                                         FunctionCodegenContext& context) const {
    if (typeNeedsDrop(dest.type, context)) {
      if (!dest.ownedSlot.empty()) {
        emitDropLocal(dest, emitter, context);
      } else {
        const std::string oldValue = memory_.emitBufferLoad(dest.type, dest.slot, emitter);
        emitDropValue(Value{oldValue, dest.type, true}, emitter, context);
      }
      if (!rvalue.owned) {
        rvalue = emitCloneValue(rvalue, emitter, context);
      }
    }
    emitStoreManagedLocal(dest, rvalue, emitter, context);
  }

} // namespace noria::codegen_detail
