#include "CodegenInternal.hpp"

#include "noria/Diagnostic.hpp"

#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>

namespace noria::codegen_detail {

  std::unordered_map<std::string, StructLayout>
  StructEmitter::collectStructLayouts(const ast::Module& module) const {
    std::unordered_map<std::string, StructLayout> layouts;

    for (const auto& decl : module.structs) {
      if (!decl.typeParams.empty()) {
        continue;
      }
      StructLayout layout;
      layout.fieldIndex.reserve(decl.fields.size());
      for (const auto& field : decl.fields) {
        const std::size_t index = layout.fieldTypes.size();
        layout.fieldNames.push_back(field.name);
        layout.fieldTypes.push_back(field.type);
        layout.fieldIndex.emplace(field.name, index);
      }
      layouts.emplace(decl.name, std::move(layout));
    }

    return layouts;
  }

  std::string StructEmitter::emitStructTypeDefinitions(const ast::Module& module) const {
    std::ostringstream out;

    for (const auto& decl : module.structs) {
      if (!decl.typeParams.empty()) {
        continue;
      }
      out << "%" << decl.name << " = type { ";
      for (std::size_t index{}; index < decl.fields.size(); ++index) {
        if (index != 0) {
          out << ", ";
        }
        out << LLVMType(decl.fields[index].type);
      }
      out << " }\n";
    }

    return out.str();
  }

  const StructLayout& StructEmitter::lookupStructLayout(const FunctionCodegenContext& context,
                                                        const Type& structType) const {
    const auto layout = context.module.structs.find(structType.structName());
    if (layout == context.module.structs.end()) {
      throw CompileError("codegen: unknown struct '" + structType.structName() + "'");
    }

    return layout->second;
  }

  std::string StructEmitter::emitStructFieldPointer(const Type& structType, const std::string& slot,
                                                    std::size_t fieldIndex,
                                                    IREmitter& emitter) const {
    const std::string pointer = emitter.freshTemp();
    emitter.line(pointer + " = getelementptr inbounds " + LLVMType(structType) + ", ptr " + slot +
                 ", i32 0, i32 " + std::to_string(fieldIndex));
    return pointer;
  }

  Value StructEmitter::generateStructLiteral(const ExpressionEmitter& expressions,
                                             const OwnershipEmitter& ownership,
                                             const ast::StructLiteral& literal, IREmitter& emitter,
                                             FunctionCodegenContext& context) const {
    const Type structType = Type::structType(literal.structName);
    const StructLayout& layout = lookupStructLayout(context, structType);

    std::unordered_map<std::string, const ast::Expression*> fieldValues;
    for (const auto& field : literal.fields) {
      fieldValues.emplace(field.name, field.value.get());
    }

    const std::string slot = emitter.freshTemp();
    emitter.emitAlloca(structType, slot);

    for (std::size_t index{}; index < layout.fieldTypes.size(); ++index) {
      const std::string& fieldName = layout.fieldNames[index];
      const auto valueExpression = fieldValues.find(fieldName);
      if (valueExpression == fieldValues.end()) {
        throw CompileError("codegen: missing struct literal field '" + fieldName + "'");
      }

      const Value fieldValue = expressions.generateRvalue(*valueExpression->second, emitter,
                                                          context, layout.fieldTypes[index]);
      const std::string pointer = emitStructFieldPointer(structType, slot, index, emitter);
      emitter.emitStore(layout.fieldTypes[index], fieldValue.text, pointer);
    }

    const std::string result = emitter.freshTemp();
    emitter.emitLoad(structType, slot, result);
    return Value{result, structType, ownership.typeNeedsDrop(structType, context)};
  }

  Value StructEmitter::generateFieldAccess(const ExpressionEmitter& expressions,
                                           const OwnershipEmitter& ownership,
                                           const ast::FieldAccessExpression& access,
                                           IREmitter& emitter, FunctionCodegenContext& context,
                                           LLVMGenerator::OwnershipMode ownershipMode) const {
    const Value base = expressions.generateRvalue(*access.base, emitter, context, std::nullopt,
                                                  LLVMGenerator::OwnershipMode::Borrow);
    if (base.type.kind() != TypeKind::Struct) {
      throw CompileError("codegen: field access requires struct base");
    }

    const std::string slot = emitter.freshTemp();
    emitter.emitAlloca(base.type, slot);
    emitter.emitStore(base.type, base.text, slot);

    const StructLayout& layout = lookupStructLayout(context, base.type);
    const auto fieldIndex = layout.fieldIndex.find(access.fieldName);
    if (fieldIndex == layout.fieldIndex.end()) {
      throw CompileError("codegen: struct '" + base.type.structName() + "' has no field '" +
                         access.fieldName + "'");
    }

    const Type fieldType = layout.fieldTypes[fieldIndex->second];
    const std::string pointer =
        emitStructFieldPointer(base.type, slot, fieldIndex->second, emitter);
    const std::string result = emitter.freshTemp();
    emitter.emitLoad(fieldType, pointer, result);
    Value field{result, fieldType, false};
    if (ownership.typeNeedsDrop(fieldType, context) &&
        (ownershipMode == LLVMGenerator::OwnershipMode::Own || base.owned)) {
      field = ownership.emitCloneValue(field, emitter, context);
    }
    ownership.emitReleaseIfOwned(base, emitter, context);
    return field;
  }

} // namespace noria::codegen_detail
