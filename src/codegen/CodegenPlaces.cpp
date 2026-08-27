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

  LLVMGenerator::PlacesState::PlaceVisitor::PlaceVisitor(const PlacesState& state,
                                                  IREmitter& emitter,
                                                  FunctionCodegenContext& context,
                                                  const std::vector<Scope>& scopes)
      : state_(state), emitter_(emitter), context_(context), scopes_(scopes) {}

  void LLVMGenerator::PlacesState::PlaceVisitor::visit(const ast::IdentifierExpression& identifier) {
    result_ = state_.generator().lookupLocal(scopes_, identifier.name);
  }

  void LLVMGenerator::PlacesState::PlaceVisitor::visit(const ast::IntegerLiteral&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlacesState::PlaceVisitor::visit(const ast::FloatLiteral&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlacesState::PlaceVisitor::visit(const ast::StringLiteral&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlacesState::PlaceVisitor::visit(const ast::BoolLiteral&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlacesState::PlaceVisitor::visit(const ast::UnaryExpression&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlacesState::PlaceVisitor::visit(const ast::CastExpression&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlacesState::PlaceVisitor::visit(const ast::BinaryExpression&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlacesState::PlaceVisitor::visit(const ast::CallExpression&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlacesState::PlaceVisitor::visit(const ast::ArrayLiteral&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlacesState::PlaceVisitor::visit(const ast::StructLiteral&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlacesState::PlaceVisitor::visit(const ast::IndexExpression& index) {
    const Value base = state_.generator().generateRvalue(*index.base, emitter_, context_, scopes_,
                                                         std::nullopt,
                                                         LLVMGenerator::OwnershipMode::Borrow);
    const Value indexValue = state_.generator().generateRvalue(*index.index, emitter_, context_, scopes_);

    if (base.type.kind != TypeKind::Array) {
      throw CompileError("codegen: invalid assignment target");
    }
    if (!base.type.element) {
      throw CompileError("codegen: array type missing element type");
    }

    const Type elementType = *base.type.element;
    const std::string pointer =
        state_.emitArrayElementPointer(base, indexValue, elementType, emitter_, context_);
    result_ = LocalBinding{pointer, elementType, true};
  }

  void LLVMGenerator::PlacesState::PlaceVisitor::visit(const ast::FieldAccessExpression& access) {
    const LocalBinding base = state_.generatePlace(*access.base, emitter_, context_, scopes_);
    if (base.type.kind != TypeKind::Struct) {
      throw CompileError("codegen: field access requires struct base");
    }

    const StructLayout& layout = state_.generator().lookupStructLayout(context_, base.type);
    const auto field = layout.fieldIndex.find(access.fieldName);
    if (field == layout.fieldIndex.end()) {
      throw CompileError("codegen: struct '" + base.type.structName + "' has no field '" +
                         access.fieldName + "'");
    }

    result_ = LocalBinding{
        state_.generator().emitStructFieldPointer(base.type, base.slot, field->second, emitter_),
        layout.fieldTypes[field->second]};
  }

  void LLVMGenerator::PlacesState::PlaceVisitor::visit(const ast::ReturnStatement&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlacesState::PlaceVisitor::visit(const ast::LetStatement&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlacesState::PlaceVisitor::visit(const ast::IfStatement&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlacesState::PlaceVisitor::visit(const ast::WhileStatement&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlacesState::PlaceVisitor::visit(const ast::AssignmentStatement&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlacesState::PlaceVisitor::visit(const ast::ExpressionStatement&) {
    throw CompileError("codegen: invalid assignment target");
  }

  LLVMGenerator::LocalBinding
  LLVMGenerator::PlacesState::generatePlace(const ast::Expression& place, IREmitter& emitter,
                                     FunctionCodegenContext& context,
                               const std::vector<Scope>& scopes) const {
    PlaceVisitor visitor(*this, emitter, context, scopes);
    place.accept(visitor);
    return visitor.result();
  }

  std::string LLVMGenerator::PlacesState::emitArrayElementPointer(const Value& base,
                                                           const Value& indexValue,
                                                           const Type& elementType,
                                                           IREmitter& emitter,
                                                           FunctionCodegenContext& context) const {
    const std::string length = emitter.freshTemp();
    emitter.line(length + " = load i64, ptr " + base.text);
    emitBoundsCheck(length, indexValue, emitter, context, "array index out of bounds\n");

    const std::string elems = emitter.freshTemp();
    emitter.line(elems + " = getelementptr inbounds i8, ptr " + base.text + ", i64 8");
    return emitRawBufferElementPointer(Value{elems, Type::rawPtr()}, indexValue, elementType,
                                       emitter);
  }

  std::string LLVMGenerator::PlacesState::emitCStringPointer(std::string_view text, IREmitter& emitter,
                                                      FunctionCodegenContext& context) const {
    const std::string globalName = "@.str." + std::to_string(context.nextStringGlobal++);
    const std::size_t length = text.size() + 1;
    context.globals << globalName << " = private unnamed_addr constant { i32, [" << length
                    << " x i8] } { i32 -1, [" << length << " x i8] c\""
                    << escapeForLLVMString(text) << "\\00\" }\n";

    const std::string result = emitter.freshTemp();
    emitter.line(result + " = getelementptr inbounds { i32, [" + std::to_string(length) +
                 " x i8] }, ptr " + globalName + ", i32 0, i32 1");
    return result;
  }

  bool LLVMGenerator::PlacesState::typeNeedsDrop(const Type& type,
                                                  const FunctionCodegenContext& context) const {
    if (type.kind == TypeKind::Str || type.kind == TypeKind::Array) {
      return true;
    }
    if (type.kind != TypeKind::Struct) {
      return false;
    }
    if (standardContainerKindFromStructName(type.structName)) {
      return false;
    }

    const StructLayout& layout = generator().lookupStructLayout(context, type);
    for (const Type& fieldType : layout.fieldTypes) {
      if (typeContainsManaged(fieldType, context)) {
        return true;
      }
    }
    return false;
  }

  bool LLVMGenerator::PlacesState::typeContainsManaged(const Type& type,
                                                        const FunctionCodegenContext& context) const {
    return typeNeedsDrop(type, context);
  }

  void LLVMGenerator::PlacesState::emitReleaseIfOwned(const Value& value, IREmitter& emitter,
                                                       FunctionCodegenContext& context) const {
    if (value.owned) {
      emitDropValue(value, emitter, context);
    }
  }

  void LLVMGenerator::PlacesState::emitDropValue(const Value& value, IREmitter& emitter,
                                                  FunctionCodegenContext& context) const {
    if (value.type == Type::str()) {
      emitter.line("call void @__noria.rt.drop_str(ptr " + value.text + ")");
      return;
    }

    if (value.type.kind == TypeKind::Array) {
      if (!value.type.element) {
        throw CompileError("codegen: array type missing element type");
      }

      const Type elementType = *value.type.element;
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
        const std::string elementPointer = emitRawBufferElementPointer(
            Value{elems, Type::rawPtr()}, indexValue, elementType, emitter);
        const std::string elementText = emitBufferLoad(elementType, elementPointer, emitter);
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

    if (value.type.kind == TypeKind::Struct) {
      if (standardContainerKindFromStructName(value.type.structName)) {
        return;
      }

      const std::string slot = emitter.freshTemp();
      emitter.emitAlloca(value.type, slot);
      emitter.emitStore(value.type, value.text, slot);
      const StructLayout& layout = generator().lookupStructLayout(context, value.type);
      for (std::size_t index{}; index < layout.fieldTypes.size(); ++index) {
        const Type& fieldType = layout.fieldTypes[index];
        if (!typeContainsManaged(fieldType, context)) {
          continue;
        }
        const std::string fieldPointer =
            generator().emitStructFieldPointer(value.type, slot, index, emitter);
        const std::string fieldText = emitBufferLoad(fieldType, fieldPointer, emitter);
        emitDropValue(Value{fieldText, fieldType, true}, emitter, context);
      }
    }
  }

  LLVMGenerator::Value LLVMGenerator::PlacesState::emitCloneValue(const Value& value,
                                                                     IREmitter& emitter,
                                                                     FunctionCodegenContext& context) const {
    if (value.type == Type::str()) {
      const std::string cloned = emitter.freshTemp();
      emitter.line(cloned + " = call ptr @__noria.rt.clone_str(ptr " + value.text + ")");
      return Value{cloned, Type::str(), true};
    }

    if (value.type.kind == TypeKind::Array) {
      if (!value.type.element) {
        throw CompileError("codegen: array type missing element type");
      }

      const Type elementType = *value.type.element;
      const std::string length = emitter.freshTemp();
      emitter.line(length + " = load i64, ptr " + value.text);
      const std::string payloadBytes = emitter.freshTemp();
      emitter.line(payloadBytes + " = mul i64 " + length + ", " +
                   std::to_string(elementSizeInBytes(elementType)));
      const std::string totalBytes = emitter.freshTemp();
      emitter.line(totalBytes + " = add i64 " + payloadBytes + ", 8");
      const std::string cloneBase = emitCheckedMalloc(totalBytes, emitter, context);
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
      const std::string sourcePointer = emitRawBufferElementPointer(
          Value{sourceElements, Type::rawPtr()}, indexValue, elementType, emitter);
      const std::string sourceText = emitBufferLoad(elementType, sourcePointer, emitter);
      const Value clonedElement =
          emitCloneValue(Value{sourceText, elementType, true}, emitter, context);
      const std::string destPointer = emitRawBufferElementPointer(
          Value{destElements, Type::rawPtr()}, indexValue, elementType, emitter);
      emitBufferStore(elementType, clonedElement.text, destPointer, emitter);

      const std::string nextIndex = emitter.freshTemp();
      emitter.line(nextIndex + " = add i64 " + index64 + ", 1");
      emitter.line("store i64 " + nextIndex + ", ptr " + indexSlot);
      emitter.emitBranch(conditionLabel);
      emitter.emitLabel(endLabel);
      return Value{cloneBase, value.type, true};
    }

    if (value.type.kind == TypeKind::Struct) {
      if (standardContainerKindFromStructName(value.type.structName)) {
        return value;
      }

      const std::string slot = emitter.freshTemp();
      emitter.emitAlloca(value.type, slot);
      emitter.emitStore(value.type, value.text, slot);
      const StructLayout& layout = generator().lookupStructLayout(context, value.type);
      for (std::size_t index{}; index < layout.fieldTypes.size(); ++index) {
        const Type& fieldType = layout.fieldTypes[index];
        if (!typeContainsManaged(fieldType, context)) {
          continue;
        }
        const std::string fieldPointer =
            generator().emitStructFieldPointer(value.type, slot, index, emitter);
        const std::string fieldText = emitBufferLoad(fieldType, fieldPointer, emitter);
        const Value clonedField =
            emitCloneValue(Value{fieldText, fieldType, true}, emitter, context);
        emitBufferStore(fieldType, clonedField.text, fieldPointer, emitter);
      }

      const std::string result = emitter.freshTemp();
      emitter.emitLoad(value.type, slot, result);
      return Value{result, value.type, true};
    }

    return value;
  }

  void LLVMGenerator::PlacesState::emitDropLocal(const LocalBinding& local, IREmitter& emitter,
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

  void LLVMGenerator::PlacesState::emitDropScope(Scope& scope, IREmitter& emitter,
                                                  FunctionCodegenContext& context) const {
    if (!scope.containsPtr) {
      return;
    }

    for (const auto& [name, binding] : scope.bindings) {
      (void)name;
      emitDropLocal(binding, emitter, context);
    }
  }

  void LLVMGenerator::PlacesState::emitDropScopes(std::vector<Scope>& scopes, IREmitter& emitter,
                                                   FunctionCodegenContext& context) const {
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
      emitDropScope(*scope, emitter, context);
    }
  }

  void LLVMGenerator::PlacesState::emitStoreManagedLocal(const LocalBinding& local,
                                                          const Value& value, IREmitter& emitter,
                                                          FunctionCodegenContext& context) const {
    if (local.byteBuffer) {
      emitBufferStore(local.type, value.text, local.slot, emitter);
    } else {
      emitter.emitStore(local.type, value.text, local.slot);
    }

    if (!local.ownedSlot.empty()) {
      const std::string ownedValue = value.owned ? "true" : "false";
      emitter.line("store i1 " + ownedValue + ", ptr " + local.ownedSlot);
    }
  }

  void LLVMGenerator::PlacesState::emitRuntimeTrap(IREmitter& emitter, FunctionCodegenContext& context,
                                            std::string_view message) const {
    const std::string pointer = emitCStringPointer(message, emitter, context);
    emitter.line("call void @\"__noria.rt.trap\"(ptr " + pointer + ")");
    emitter.line("unreachable");
  }

  void LLVMGenerator::PlacesState::emitTrapUnless(const std::string& condition, std::string_view labelPrefix,
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

  void LLVMGenerator::PlacesState::emitNullPointerCheck(const std::string& pointer, IREmitter& emitter,
                                                 FunctionCodegenContext& context) const {
    const std::string isNonNull = emitter.freshTemp();
    emitter.line(isNonNull + " = icmp ne ptr " + pointer + ", null");
    emitTrapUnless(isNonNull, "alloc", emitter, context, "allocation failed\n");
  }

  std::string LLVMGenerator::PlacesState::emitCheckedMalloc(const std::string& size64, IREmitter& emitter,
                                                     FunctionCodegenContext& context) const {
    const std::string pointer = emitter.freshTemp();
    emitter.line(pointer + " = call ptr @malloc(i64 " + size64 + ")");
    emitNullPointerCheck(pointer, emitter, context);
    return pointer;
  }

  void LLVMGenerator::PlacesState::emitBoundsCheck(const std::string& length64, const Value& indexValue,
                                            IREmitter& emitter, FunctionCodegenContext& context,
                                            std::string_view message) const {
    const std::string index64 = emitter.freshTemp();
    emitter.line(index64 + " = zext i32 " + indexValue.text + " to i64");
    const std::string inBounds = emitter.freshTemp();
    emitter.line(inBounds + " = icmp ult i64 " + index64 + ", " + length64);
    emitTrapUnless(inBounds, "bounds", emitter, context, message);
  }

  std::string LLVMGenerator::PlacesState::emitRawBufferElementPointer(const Value& base,
                                                               const Value& indexValue,
                                                               const Type& elementType,
                                                               IREmitter& emitter) const {
    const std::size_t size = elementSizeInBytes(elementType);
    const std::string offset = emitter.freshTemp();
    emitter.line(offset + " = mul i32 " + indexValue.text + ", " + std::to_string(size));
    const std::string pointer = emitter.freshTemp();
    emitter.line(pointer + " = getelementptr i8, ptr " + base.text + ", i32 " + offset);
    return pointer;
  }

  std::string LLVMGenerator::PlacesState::emitBufferLoad(const Type& type, const std::string& pointer,
                                                  IREmitter& emitter) const {
    if (type.kind == TypeKind::Bool) {
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

  void LLVMGenerator::PlacesState::emitBufferStore(const Type& type, const std::string& value,
                                            const std::string& pointer, IREmitter& emitter) const {
    if (type.kind == TypeKind::Bool) {
      const std::string packed = emitter.freshTemp();
      emitter.line(packed + " = zext i1 " + value + " to i8");
      emitter.line("store i8 " + packed + ", ptr " + pointer);
      return;
    }

    emitter.line("store " + LLVMType(type) + " " + value + ", ptr " + pointer);
  }

} // namespace noria
