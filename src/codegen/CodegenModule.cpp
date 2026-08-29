#include "CodegenInternal.hpp"

#include "noria/Diagnostic.hpp"
#include "noria/Runtime.hpp"
#include "noria/SemanticTables.hpp"

#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace noria::codegen_detail {

  std::string ModuleEmitter::generateModule(
      const ast::Module& module,
      const std::unordered_map<std::string, std::vector<Type>>& functionTypeArgs,
      const std::unordered_map<std::string, std::vector<Type>>& structTypeArgs) const {
    ModuleCodegenContext context(functionTypeArgs, structTypeArgs);
    context.functions = collectFunctionBindings(module);
    context.structs = structs_.collectStructLayouts(module);

    std::ostringstream functions;
    for (const auto& function : module.functions) {
      if (!function.typeParams.empty()) {
        continue;
      }
      functions << generateFunction(function, context) << "\n";
    }

    return modulePreamble() + structs_.emitStructTypeDefinitions(module) + context.globals.str() +
           functions.str();
  }

  std::string ModuleEmitter::modulePreamble() const {
    std::string preamble;

    const std::string triple = runtime::targetTriple();
    if (!triple.empty()) {
      preamble += "target triple = \"" + triple + "\"\n";
      preamble += "target datalayout = \"" + runtime::targetDataLayout() + "\"\n";
    }

    for (const std::string_view declaration : runtime::runtimeDeclarations) {
      preamble += declaration;
    }

    for (const std::string_view global : runtime::runtimeGlobals) {
      preamble += global;
    }

    preamble += "\n";
    preamble += runtime::runtimeDefinitions;
    const std::string_view trapDefinition = runtime::runtimeTrapDefinition();
    if (!trapDefinition.empty()) {
      preamble += trapDefinition;
    }
    return preamble;
  }

  std::string ModuleEmitter::defaultIRValue(const Type& type) const {
    if (type == Type::boolean()) {
      return "false";
    }
    if (type == Type::f64()) {
      return "0.0";
    }
    if (type.kind() == TypeKind::I32) {
      return "0";
    }
    if (type.kind() == TypeKind::RawPtr) {
      return "null";
    }
    throw CompileError("codegen: type '" + type.name() + "' has no constant default IR value");
  }

  Value ModuleEmitter::emitDefaultValue(const Type& type, IREmitter& emitter,
                                        FunctionCodegenContext& context) const {
    Value value;
    if (type.kind() == TypeKind::Str) {
      value = Value{memory_.emitCStringPointer("", emitter, context), Type::str(), false};
    } else if (type.kind() == TypeKind::Array) {
      const std::string base = memory_.emitCheckedMalloc("8", emitter, context);
      emitter.line("store i64 0, ptr " + base);
      value = Value{base, type, true};
    } else if (type.kind() == TypeKind::Struct) {
      if (const std::optional<StandardContainer> container =
              standardContainerKindFromStructName(type.structName())) {
        const std::vector<Type> typeArgs = specializedStructTypeArgs(type, context);
        std::vector<Value> samples;
        if (*container == StandardContainer::Dictionary) {
          if (typeArgs.size() < 2) {
            throw CompileError("codegen: dictionary default is missing type arguments");
          }
          samples.push_back(emitDefaultValue(typeArgs[0], emitter, context));
          samples.push_back(emitDefaultValue(typeArgs[1], emitter, context));
        } else {
          if (typeArgs.empty()) {
            throw CompileError("codegen: container default is missing type arguments");
          }
          samples.push_back(emitDefaultValue(typeArgs[0], emitter, context));
        }
        value = emitStandardContainerCall(*container, ContainerOperation::New, typeArgs, samples,
                                          emitter, context);
      } else {
        const std::string slot = emitter.freshTemp();
        emitter.emitAlloca(type, slot);
        emitDefaultStore(type, slot, emitter, context);
        const std::string result = emitter.freshTemp();
        emitter.emitLoad(type, slot, result);
        value = Value{result, type};
      }
    } else {
      value = Value{defaultIRValue(type), type};
    }

    value.owned = ownership_.typeNeedsDrop(type, context);
    return value;
  }

  void ModuleEmitter::emitDefaultStore(const Type& type, const std::string& slot,
                                       IREmitter& emitter, FunctionCodegenContext& context) const {
    if (type.kind() == TypeKind::Struct) {
      if (standardContainerKindFromStructName(type.structName())) {
        const Value value = emitDefaultValue(type, emitter, context);
        emitter.emitStore(type, value.text, slot);
        return;
      }

      const StructLayout& layout = structs_.lookupStructLayout(context, type);
      for (std::size_t index{}; index < layout.fieldTypes.size(); ++index) {
        const std::string fieldPointer =
            structs_.emitStructFieldPointer(type, slot, index, emitter);
        emitDefaultStore(layout.fieldTypes[index], fieldPointer, emitter, context);
      }
      return;
    }

    const Value value = emitDefaultValue(type, emitter, context);
    emitter.emitStore(type, value.text, slot);
  }

  std::unordered_map<std::string, FunctionBinding>
  ModuleEmitter::collectFunctionBindings(const ast::Module& module) const {
    std::unordered_map<std::string, FunctionBinding> functions;

    for (const auto& function : module.functions) {
      if (!function.typeParams.empty()) {
        continue;
      }

      FunctionBinding binding;
      if (!function.returnType) {
        throw CompileError("codegen: function '" + function.name +
                           "' has an unresolved return type");
      }
      binding.returnType = *function.returnType;
      for (const auto& parameter : function.parameters) {
        binding.parameterTypes.push_back(parameter.type);
      }
      functions.emplace(function.name, std::move(binding));
    }

    return functions;
  }

  std::string ModuleEmitter::generateFunction(const ast::Function& function,
                                              ModuleCodegenContext& moduleContext) const {
    FunctionCodegenContext context(moduleContext, function.name);
    if (!function.returnType) {
      throw CompileError("codegen: function '" + function.name + "' has an unresolved return type");
    }
    const Type returnType = *function.returnType;

    std::ostringstream out;
    IREmitter emitter(out);
    out << "define " << LLVMType(returnType) << " @" << function.name << "(";
    for (std::size_t index{}; index < function.parameters.size(); ++index) {
      const auto& parameter = function.parameters[index];
      const Type parameterType = parameter.type;

      if (index != 0) {
        out << ", ";
      }

      out << LLVMType(parameterType) << " %" << parameter.name << ".param";
    }
    out << ") {\n";
    out << "entry:\n";

    context.scopes.emplace_back();

    for (const auto& parameter : function.parameters) {
      const Type parameterType = parameter.type;
      LocalBinding binding{"%" + parameter.name, parameterType, false, {}};
      if (ownership_.typeNeedsDrop(parameterType, context)) {
        binding.ownedSlot = "%" + parameter.name + ".owned";
        emitter.emitAlloca(Type::boolean(), binding.ownedSlot);
        emitter.line("store i1 false, ptr " + binding.ownedSlot);
      }

      if (!context.declareLocal(parameter.name, std::move(binding),
                                ownership_.typeContainsManaged(parameterType, context))) {
        throw CompileError("codegen: duplicate parameter '" + parameter.name + "'");
      }

      const std::string slot = "%" + parameter.name;
      emitter.emitAlloca(parameterType, slot);
      emitter.emitStore(parameterType, "%" + parameter.name + ".param", slot);
    }

    const bool emittedReturn =
        statements_.generateStatements(function.body, emitter, context, returnType);

    if (!emittedReturn) {
      throw CompileError("codegen: function '" + function.name +
                         "' reached code generation without an explicit return");
    }

    out << "}\n";
    return out.str();
  }

} // namespace noria::codegen_detail
