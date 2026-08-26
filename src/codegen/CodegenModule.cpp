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

  std::string LLVMGenerator::ModuleState::generateModule(const ast::Module& module) const {
    ModuleCodegenContext context(generator().functionSpecializationTypeArgs_,
                                 generator().structSpecializationTypeArgs_);
    context.functions = generator().collectFunctionBindings(module);
    context.structs = generator().collectStructLayouts(module);

    std::ostringstream functions;
    for (const auto& function : module.functions) {
      if (!function.typeParams.empty()) {
        continue;
      }
      functions << generateFunction(function, context) << "\n";
    }

    return modulePreamble() + generator().emitStructTypeDefinitions(module) +
           context.globals.str() +
           functions.str();
  }

  std::string LLVMGenerator::ModuleState::modulePreamble() const {
    std::string preamble;

    const std::string triple = runtime::targetTriple();
    if (!triple.empty()) {
      preamble += "target triple = \"" + triple + "\"\n";
      preamble += "target datalayout = \"" + runtime::targetDataLayout() + "\"\n";
    }

    for (const std::string_view declaration : runtime::runtimeDeclarations)
      preamble += declaration;

    for (const std::string_view global : runtime::runtimeGlobals)
      preamble += global;

    preamble += "\n";
    preamble += runtime::runtimeDefinitions;
    const std::string_view trapDefinition = runtime::runtimeTrapDefinition();
    if (!trapDefinition.empty())
      preamble += trapDefinition;
    return preamble;
  }

  std::string LLVMGenerator::ModuleState::defaultIRValue(const Type& type) const {
    if (type == Type::boolean())
      return "false";
    if (type == Type::f64())
      return "0.0";
    if (type.kind == TypeKind::I32)
      return "0";
    if (type.kind == TypeKind::RawPtr)
      return "null";
    throw CompileError("codegen: type '" + type.name() + "' has no constant default IR value");
  }

  LLVMGenerator::Value
  LLVMGenerator::ModuleState::emitDefaultValue(const Type& type, IREmitter& emitter,
                                                FunctionCodegenContext& context) const {
    if (type.kind == TypeKind::Str) {
      return Value{emitCStringPointer("", emitter, context), Type::str()};
    }

    if (type.kind == TypeKind::Array) {
      if (!type.element) {
        throw CompileError("codegen: array type missing element type");
      }
      const std::string base = emitCheckedMalloc("8", emitter, context);
      emitter.line("store i64 0, ptr " + base);
      return Value{base, type};
    }

    if (type.kind == TypeKind::Struct) {
      if (const std::optional<StandardContainer> container =
              standardContainerKindFromStructName(type.structName)) {
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
        return emitStandardContainerCall(*container, ContainerOperation::New, typeArgs, samples,
                                         emitter, context);
      }

      const std::string slot = emitter.freshTemp();
      emitter.emitAlloca(type, slot);
      emitDefaultStore(type, slot, emitter, context);
      const std::string result = emitter.freshTemp();
      emitter.emitLoad(type, slot, result);
      return Value{result, type};
    }

    return Value{defaultIRValue(type), type};
  }

  void LLVMGenerator::ModuleState::emitDefaultStore(const Type& type, const std::string& slot,
                                                     IREmitter& emitter,
                                                     FunctionCodegenContext& context) const {
    if (type.kind == TypeKind::Struct) {
      if (standardContainerKindFromStructName(type.structName)) {
        const Value value = emitDefaultValue(type, emitter, context);
        emitter.emitStore(type, value.text, slot);
        return;
      }

      const StructLayout& layout = lookupStructLayout(context, type);
      for (std::size_t index{}; index < layout.fieldTypes.size(); ++index) {
        const std::string fieldPointer = emitStructFieldPointer(type, slot, index, emitter);
        emitDefaultStore(layout.fieldTypes[index], fieldPointer, emitter, context);
      }
      return;
    }

    const Value value = emitDefaultValue(type, emitter, context);
    emitter.emitStore(type, value.text, slot);
  }

  std::string LLVMGenerator::ModuleState::generateFunction(
      const ast::Function& function, ModuleCodegenContext& moduleContext) const {
    FunctionCodegenContext context(moduleContext, function.name);
    if (!function.returnType) {
      throw CompileError("codegen: function '" + function.name +
                         "' has an unresolved return type");
    }
    const Type returnType = *function.returnType;

    std::ostringstream out;
    IREmitter emitter(out);
    out << "define " << LLVMType(returnType) << " @" << function.name << "(";
    for (std::size_t index{}; index < function.parameters.size(); ++index) {
      const auto& parameter = function.parameters[index];
      const Type parameterType = parameter.type;

      if (index != 0)
        out << ", ";

      out << LLVMType(parameterType) << " %" << parameter.name << ".param";
    }
    out << ") {\n";
    out << "entry:\n";

    context.scopes.emplace_back(); // scope is an unordered_map, create an empty scope

    for (const auto& parameter : function.parameters) {
      const Type parameterType = parameter.type;
      if (!generator().declareLocal(context.scopes, parameter.name,
                                    LocalBinding{"%" + parameter.name, parameterType})) {
        throw CompileError("codegen: duplicate parameter '" + parameter.name + "'");
      }

      const std::string slot = "%" + parameter.name;
      emitter.emitAlloca(parameterType, slot);
      emitter.emitStore(parameterType, "%" + parameter.name + ".param", slot);
    }

    const bool emittedReturn =
        generator().generateStatements(function.body, emitter, context, returnType,
                                       context.scopes);

    if (!emittedReturn) {
      throw CompileError("codegen: function '" + function.name +
                         "' reached code generation without an explicit return");
    }

    out << "}\n";
    return out.str();
  }

} // namespace noria
