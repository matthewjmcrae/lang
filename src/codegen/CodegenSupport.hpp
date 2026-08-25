#pragma once

#include "../internal/SpecializationSupport.hpp"
#include "noria/Types.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace noria::codegen_detail {

  std::string formatLLVMFloatLiteral(double value);
  std::string escapeForLLVMString(std::string_view value);
  std::size_t elementSizeInBytes(const Type& type);
  using internal::firstNonImplTagTypeArg;

  Type resolveWitnessType(
      const std::unordered_map<std::string, std::vector<Type>>& specializationTypeArgs,
      std::string_view currentFunctionName);

} // namespace noria::codegen_detail
