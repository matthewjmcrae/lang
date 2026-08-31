#include "CodegenSupport.hpp"

#include "CodegenInternal.hpp"

#include "noria/Diagnostic.hpp"
#include "noria/Monomorphize.hpp"
#include "noria/SemanticTables.hpp"
#include <array>
#include <charconv>
#include <limits>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace noria {

  namespace codegen_detail {
    std::string formatLLVMFloatLiteral(double value) {
      constexpr std::size_t bufferSize = std::numeric_limits<double>::max_digits10 + 16;
      std::array<char, bufferSize> buffer{};
      const auto [end, error] =
          std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                        std::chars_format::general, std::numeric_limits<double>::max_digits10);
      if (error != std::errc{}) {
        throw CompileError("codegen: internal error: unable to format f64 literal");
      }

      std::string text(buffer.data(), end);
      if (text.find('.') == std::string::npos) {
        const std::size_t exponent = text.find_first_of("eE");
        text.insert(exponent == std::string::npos ? text.size() : exponent, ".0");
      }
      return text;
    }

    std::string escapeForLLVMString(std::string_view value) {
      std::string escaped;
      for (const unsigned char character : value) {
        switch (character) {
        case '\\':
          escaped += "\\5C";
          break;
        case '"':
          escaped += "\\22";
          break;
        case '\n':
          escaped += "\\0A";
          break;
        case '\t':
          escaped += "\\09";
          break;
        default:
          if (character >= 32 && character <= 126) {
            escaped.push_back(static_cast<char>(character));
          } else {
            static constexpr char hex[] = "0123456789ABCDEF";
            escaped += '\\';
            escaped += hex[character >> 4];
            escaped += hex[character & 0xF];
          }
          break;
        }
      }
      return escaped;
    }

    std::size_t elementSizeInBytes(const Type& type) {
      if (type.kind() == TypeKind::Struct) {
        throw CompileError("codegen: struct element size is not supported");
      }

      if (type.kind() == TypeKind::ImplTag) {
        throw CompileError("codegen: internal: implementation tag is not a runtime type");
      }

      if (type.kind() == TypeKind::TypeParam) {
        throw CompileError("internal: unsubstituted type parameter");
      }

      if (const TypeKindInfo* info = typeKindInfo(type.kind()); info && info->runtimeElementSize) {
        return *info->runtimeElementSize;
      }

      throw CompileError("codegen: unsupported array element type");
    }

    Type resolveWitnessType(
        const std::unordered_map<std::string, std::vector<Type>>& specializationTypeArgs,
        std::string_view currentFunctionName) {
      const auto specialization = specializationTypeArgs.find(std::string(currentFunctionName));
      if (specialization == specializationTypeArgs.end()) {
        throw CompileError("codegen: witness-polymorphic runtime builtin requires an enclosing "
                           "generic specialization context");
      }

      const std::optional<Type> witness = firstNonImplTagTypeArg(specialization->second);
      if (!witness) {
        throw CompileError("codegen: witness-polymorphic runtime builtin requires an enclosing "
                           "generic specialization context");
      }

      return *witness;
    }

    Value emitStandardContainerCall(StandardContainer container, ContainerOperation operation,
                                    const std::vector<Type>& typeArgs,
                                    const std::vector<Value>& arguments, IREmitter& emitter,
                                    FunctionCodegenContext& context) {
      const std::string callee =
          mangleSpecialization(containerOperationHiddenName(container, operation), typeArgs);
      const auto function = context.module.functions.find(callee);
      if (function == context.module.functions.end()) {
        throw CompileError("codegen: missing container operation");
      }

      const bool returnsVoid = function->second.returnType == Type::voidType();
      const std::string result = returnsVoid ? "" : emitter.freshTemp();
      std::string call = returnsVoid ? "call void @" + callee + "("
                                     : result + " = call " + LLVMType(function->second.returnType) +
                                           " @" + callee + "(";
      for (std::size_t argument{}; argument < arguments.size(); ++argument) {
        if (argument != 0) {
          call += ", ";
        }
        call += LLVMType(arguments[argument].type) + " " + arguments[argument].text;
      }
      call += ")";
      emitter.line(call);
      Value value{result, function->second.returnType};
      if (operation == ContainerOperation::New || operation == ContainerOperation::Clone) {
        value.owned = true;
      } else if (operation == ContainerOperation::Get &&
                 function->second.returnType == Type::str()) {
        value.owned = true;
      }
      return value;
    }

    std::vector<Type> specializedStructTypeArgs(const Type& type,
                                                const FunctionCodegenContext& context) {
      if (!type.typeArguments().empty()) {
        return type.typeArguments();
      }
      const auto specialization =
          context.module.structSpecializationTypeArgs.find(type.structName());
      if (specialization == context.module.structSpecializationTypeArgs.end()) {
        return {};
      }
      return specialization->second;
    }
  } // namespace codegen_detail

} // namespace noria
