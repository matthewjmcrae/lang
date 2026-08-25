#include "CodegenInternal.hpp"

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

  std::unordered_map<std::string, LLVMGenerator::Impl::FunctionBinding>
  LLVMGenerator::Impl::collectFunctionBindings(const ast::Module& module) const {
    std::unordered_map<std::string, FunctionBinding> functions;

    for (const auto& function : module.functions) {
      if (!function.typeParams.empty()) {
        continue;
      }

      FunctionBinding binding;
      binding.returnType = function.returnType;
      for (const auto& parameter : function.parameters) {
        binding.parameterTypes.push_back(parameter.type);
      }
      functions.emplace(function.name, std::move(binding));
    }

    return functions;
  }

  std::unordered_map<std::string, LLVMGenerator::Impl::StructLayout>
  LLVMGenerator::Impl::collectStructLayouts(const ast::Module& module) const {
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

  std::string LLVMGenerator::Impl::emitStructTypeDefinitions(const ast::Module& module) const {
    std::ostringstream out;

    for (const auto& decl : module.structs) {
      if (!decl.typeParams.empty()) {
        continue;
      }
      out << "%" << decl.name << " = type { ";
      for (std::size_t index{}; index < decl.fields.size(); ++index) {
        if (index != 0)
          out << ", ";
        out << LLVMType(decl.fields[index].type);
      }
      out << " }\n";
    }

    return out.str();
  }

  const LLVMGenerator::Impl::StructLayout&
  LLVMGenerator::Impl::lookupStructLayout(const FunctionCodegenContext& context,
                                          const Type& structType) const {
    const auto layout = context.structs.find(structType.structName);
    if (layout == context.structs.end()) {
      throw CompileError("codegen: unknown struct '" + structType.structName + "'");
    }

    return layout->second;
  }

  std::string LLVMGenerator::Impl::emitStructFieldPointer(const Type& structType,
                                                          const std::string& slot,
                                                          std::size_t fieldIndex,
                                                          IREmitter& emitter) const {
    const std::string pointer = emitter.freshTemp();
    emitter.line(pointer + " = getelementptr inbounds " + LLVMType(structType) + ", ptr " + slot +
                 ", i32 0, i32 " + std::to_string(fieldIndex));
    return pointer;
  }

  LLVMGenerator::Impl::Value
  LLVMGenerator::Impl::generateStructLiteral(const ast::StructLiteral& literal, IREmitter& emitter,
                                             FunctionCodegenContext& context,
                                             const std::vector<Scope>& scopes) const {
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

      const Value fieldValue = generateRvalue(*valueExpression->second, emitter, context, scopes);
      const std::string pointer = emitStructFieldPointer(structType, slot, index, emitter);
      emitter.emitStore(layout.fieldTypes[index], fieldValue.text, pointer);
    }

    const std::string result = emitter.freshTemp();
    emitter.emitLoad(structType, slot, result);
    return Value{result, structType};
  }

  LLVMGenerator::Impl::Value
  LLVMGenerator::Impl::generateFieldAccess(const ast::FieldAccessExpression& access,
                                           IREmitter& emitter, FunctionCodegenContext& context,
                                           const std::vector<Scope>& scopes) const {
    std::string slot;
    Type structType;

    if (const auto* identifier =
            dynamic_cast<const ast::IdentifierExpression*>(access.base.get())) {
      const LocalBinding& local = lookupLocal(scopes, identifier->name);
      if (local.type.kind != TypeKind::Struct) {
        throw CompileError("codegen: field access requires struct base");
      }
      slot = local.slot;
      structType = local.type;
    } else {
      const Value baseValue = generateRvalue(*access.base, emitter, context, scopes);
      if (baseValue.type.kind != TypeKind::Struct) {
        throw CompileError("codegen: field access requires struct base");
      }
      structType = baseValue.type;
      slot = emitter.freshTemp();
      emitter.emitAlloca(structType, slot);
      emitter.emitStore(structType, baseValue.text, slot);
    }

    const StructLayout& layout = lookupStructLayout(context, structType);
    const auto fieldIndex = layout.fieldIndex.find(access.fieldName);
    if (fieldIndex == layout.fieldIndex.end()) {
      throw CompileError("codegen: struct '" + structType.structName + "' has no field '" +
                         access.fieldName + "'");
    }

    const Type fieldType = layout.fieldTypes[fieldIndex->second];
    const std::string pointer =
        emitStructFieldPointer(structType, slot, fieldIndex->second, emitter);
    const std::string result = emitter.freshTemp();
    emitter.emitLoad(fieldType, pointer, result);
    return Value{result, fieldType};
  }

} // namespace noria
