#pragma once

#include "noria/Types.hpp"

#include <string_view>
#include <vector>

namespace noria {

  enum class RequiredOperation {
    LessThan,
    Equality,
    Hash,
  };

  std::vector<RequiredOperation> requiredOperations(ImplementationTag tag);
  bool supportsOperation(const Type& type, RequiredOperation operation);
  std::string_view operationName(RequiredOperation operation);

} // namespace noria
