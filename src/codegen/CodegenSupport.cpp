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
      if (type.kind == TypeKind::Struct) {
        throw CompileError("codegen: struct element size is not supported");
      }

      if (type.kind == TypeKind::ImplTag) {
        throw CompileError("codegen: internal: implementation tag is not a runtime type");
      }

      if (type.kind == TypeKind::TypeParam) {
        throw CompileError("internal: unsubstituted type parameter");
      }

      if (const TypeKindInfo* info = typeKindInfo(type.kind); info && info->runtimeElementSize) {
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
  } // namespace codegen_detail

} // namespace noria
