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

  LLVMGenerator::PlaceVisitor::PlaceVisitor(const LLVMGenerator& generator,
                                                  IREmitter& emitter,
                                                  FunctionCodegenContext& context,
                                                  const std::vector<Scope>& scopes)
      : generator_(generator), emitter_(emitter), context_(context), scopes_(scopes) {}

  void LLVMGenerator::PlaceVisitor::visit(const ast::IdentifierExpression& identifier) {
    result_ = generator_.lookupLocal(scopes_, identifier.name);
  }

  void LLVMGenerator::PlaceVisitor::visit(const ast::IntegerLiteral&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlaceVisitor::visit(const ast::FloatLiteral&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlaceVisitor::visit(const ast::StringLiteral&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlaceVisitor::visit(const ast::BoolLiteral&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlaceVisitor::visit(const ast::UnaryExpression&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlaceVisitor::visit(const ast::CastExpression&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlaceVisitor::visit(const ast::BinaryExpression&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlaceVisitor::visit(const ast::CallExpression&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlaceVisitor::visit(const ast::ArrayLiteral&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlaceVisitor::visit(const ast::StructLiteral&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlaceVisitor::visit(const ast::IndexExpression& index) {
    const Value base = generator_.generateRvalue(*index.base, emitter_, context_, scopes_);
    const Value indexValue = generator_.generateRvalue(*index.index, emitter_, context_, scopes_);

    if (base.type.kind != TypeKind::Array) {
      throw CompileError("codegen: invalid assignment target");
    }
    if (!base.type.element) {
      throw CompileError("codegen: array type missing element type");
    }

    const Type elementType = *base.type.element;
    const std::string pointer =
        generator_.emitArrayElementPointer(base, indexValue, elementType, emitter_, context_);
    result_ = LocalBinding{pointer, elementType, true};
  }

  void LLVMGenerator::PlaceVisitor::visit(const ast::FieldAccessExpression& access) {
    const LocalBinding base = generator_.generatePlace(*access.base, emitter_, context_, scopes_);
    if (base.type.kind != TypeKind::Struct) {
      throw CompileError("codegen: field access requires struct base");
    }

    const StructLayout& layout = generator_.lookupStructLayout(context_, base.type);
    const auto field = layout.fieldIndex.find(access.fieldName);
    if (field == layout.fieldIndex.end()) {
      throw CompileError("codegen: struct '" + base.type.structName + "' has no field '" +
                         access.fieldName + "'");
    }

    result_ = LocalBinding{
        generator_.emitStructFieldPointer(base.type, base.slot, field->second, emitter_),
        layout.fieldTypes[field->second]};
  }

  void LLVMGenerator::PlaceVisitor::visit(const ast::ReturnStatement&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlaceVisitor::visit(const ast::LetStatement&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlaceVisitor::visit(const ast::IfStatement&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlaceVisitor::visit(const ast::WhileStatement&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlaceVisitor::visit(const ast::AssignmentStatement&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlaceVisitor::visit(const ast::ExpressionStatement&) {
    throw CompileError("codegen: invalid assignment target");
  }

  LLVMGenerator::LocalBinding
  LLVMGenerator::generatePlace(const ast::Expression& place, IREmitter& emitter,
                                     FunctionCodegenContext& context,
                               const std::vector<Scope>& scopes) const {
    const auto strategy = activate(CodegenStrategyKind::Places);
    PlaceVisitor visitor(*this, emitter, context, scopes);
    place.accept(visitor);
    return visitor.result();
  }

  std::string LLVMGenerator::emitArrayElementPointer(const Value& base,
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

  std::string LLVMGenerator::emitCStringPointer(std::string_view text, IREmitter& emitter,
                                                      FunctionCodegenContext& context) const {
    const std::string globalName = "@.str." + std::to_string(context.nextStringGlobal++);
    const std::size_t length = text.size() + 1;
    context.globals << globalName << " = private unnamed_addr constant [" << length << " x i8] c\""
                    << escapeForLLVMString(text) << "\\00\"\n";

    const std::string result = emitter.freshTemp();
    emitter.line(result + " = getelementptr inbounds [" + std::to_string(length) + " x i8], ptr " +
                 globalName + ", i32 0, i32 0");
    return result;
  }

  void LLVMGenerator::emitRuntimeTrap(IREmitter& emitter, FunctionCodegenContext& context,
                                            std::string_view message) const {
    const std::string pointer = emitCStringPointer(message, emitter, context);
    emitter.line("call void @\"__noria.rt.trap\"(ptr " + pointer + ")");
    emitter.line("unreachable");
  }

  void LLVMGenerator::emitNullPointerCheck(const std::string& pointer, IREmitter& emitter,
                                                 FunctionCodegenContext& context) const {
    const int labelId = emitter.freshLabelId();
    const std::string trapLabel = "alloc.fail" + std::to_string(labelId);
    const std::string contLabel = "alloc.ok" + std::to_string(labelId);
    const std::string isNull = emitter.freshTemp();
    emitter.line(isNull + " = icmp eq ptr " + pointer + ", null");
    emitter.emitCondBranch(isNull, trapLabel, contLabel);
    emitter.emitLabel(trapLabel);
    emitRuntimeTrap(emitter, context, "allocation failed\n");
    emitter.emitLabel(contLabel);
  }

  std::string LLVMGenerator::emitCheckedMalloc(const std::string& size64, IREmitter& emitter,
                                                     FunctionCodegenContext& context) const {
    const std::string pointer = emitter.freshTemp();
    emitter.line(pointer + " = call ptr @malloc(i64 " + size64 + ")");
    emitNullPointerCheck(pointer, emitter, context);
    return pointer;
  }

  void LLVMGenerator::emitBoundsCheck(const std::string& length64, const Value& indexValue,
                                            IREmitter& emitter, FunctionCodegenContext& context,
                                            std::string_view message) const {
    const int labelId = emitter.freshLabelId();
    const std::string trapLabel = "bounds.fail" + std::to_string(labelId);
    const std::string contLabel = "bounds.ok" + std::to_string(labelId);
    const std::string index64 = emitter.freshTemp();
    emitter.line(index64 + " = zext i32 " + indexValue.text + " to i64");
    const std::string inBounds = emitter.freshTemp();
    emitter.line(inBounds + " = icmp ult i64 " + index64 + ", " + length64);
    emitter.emitCondBranch(inBounds, contLabel, trapLabel);
    emitter.emitLabel(trapLabel);
    emitRuntimeTrap(emitter, context, message);
    emitter.emitLabel(contLabel);
  }

  std::string LLVMGenerator::emitRawBufferElementPointer(const Value& base,
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

  std::string LLVMGenerator::emitBufferLoad(const Type& type, const std::string& pointer,
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

  void LLVMGenerator::emitBufferStore(const Type& type, const std::string& value,
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
