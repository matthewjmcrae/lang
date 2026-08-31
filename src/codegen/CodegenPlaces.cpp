#include "CodegenInternal.hpp"

#include "noria/Diagnostic.hpp"

namespace noria::codegen_detail {

  PlaceEmitter::PlaceVisitor::PlaceVisitor(const PlaceEmitter& state, IREmitter& emitter,
                                           FunctionCodegenContext& context)
      : state_(state), emitter_(emitter), context_(context) {}

  void PlaceEmitter::PlaceVisitor::visit(const ast::IdentifierExpression& identifier) {
    result_ = context_.lookupLocal(identifier.name);
  }

  void PlaceEmitter::PlaceVisitor::visit(const ast::IntegerLiteral&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void PlaceEmitter::PlaceVisitor::visit(const ast::FloatLiteral&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void PlaceEmitter::PlaceVisitor::visit(const ast::StringLiteral&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void PlaceEmitter::PlaceVisitor::visit(const ast::BoolLiteral&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void PlaceEmitter::PlaceVisitor::visit(const ast::UnaryExpression&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void PlaceEmitter::PlaceVisitor::visit(const ast::CastExpression&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void PlaceEmitter::PlaceVisitor::visit(const ast::BinaryExpression&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void PlaceEmitter::PlaceVisitor::visit(const ast::CallExpression&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void PlaceEmitter::PlaceVisitor::visit(const ast::ArrayLiteral&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void PlaceEmitter::PlaceVisitor::visit(const ast::StructLiteral&) {
    throw CompileError("codegen: invalid assignment target");
  }

  void PlaceEmitter::PlaceVisitor::visit(const ast::IndexExpression& index) {
    const Value base = state_.expressions_.generateRvalue(
        *index.base, emitter_, context_, std::nullopt, LLVMGenerator::OwnershipMode::Borrow);
    const Value indexValue = state_.expressions_.generateRvalue(*index.index, emitter_, context_);

    if (base.type.kind() != TypeKind::Array) {
      throw CompileError("codegen: invalid assignment target");
    }
    const Type elementType = base.type.elementType();
    const std::string pointer =
        state_.memory_.emitArrayElementPointer(base, indexValue, elementType, emitter_, context_);
    result_ = LocalBinding{pointer, elementType, true, {}};
  }

  void PlaceEmitter::PlaceVisitor::visit(const ast::FieldAccessExpression& access) {
    const LocalBinding base = state_.generatePlace(*access.base, emitter_, context_);
    if (base.type.kind() != TypeKind::Struct) {
      throw CompileError("codegen: field access requires struct base");
    }

    const StructLayout& layout = state_.structs_.lookupStructLayout(context_, base.type);
    const auto field = layout.fieldIndex.find(access.fieldName);
    if (field == layout.fieldIndex.end()) {
      throw CompileError("codegen: struct '" + base.type.structName() + "' has no field '" +
                         access.fieldName + "'");
    }

    result_ = LocalBinding{
        state_.structs_.emitStructFieldPointer(base.type, base.slot, field->second, emitter_),
        layout.fieldTypes[field->second],
        false,
        {}};
  }

  void PlaceEmitter::PlaceVisitor::visit(const ast::ReturnStatement&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void PlaceEmitter::PlaceVisitor::visit(const ast::LetStatement&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void PlaceEmitter::PlaceVisitor::visit(const ast::IfStatement&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void PlaceEmitter::PlaceVisitor::visit(const ast::WhileStatement&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void PlaceEmitter::PlaceVisitor::visit(const ast::AssignmentStatement&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void PlaceEmitter::PlaceVisitor::visit(const ast::ExpressionStatement&) {
    throw CompileError("codegen: invalid assignment target");
  }

  LocalBinding PlaceEmitter::generatePlace(const ast::Expression& place, IREmitter& emitter,
                                           FunctionCodegenContext& context) const {
    PlaceVisitor visitor(*this, emitter, context);
    place.accept(visitor);
    return visitor.result();
  }

} // namespace noria::codegen_detail
