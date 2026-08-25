#pragma once

#include "noria/Types.hpp"

#include <optional>
#include <vector>

namespace noria::internal {

  std::optional<Type> firstNonImplTagTypeArg(const std::vector<Type>& typeArgs);
  std::optional<ImplementationTag> findImplTag(const std::vector<Type>& typeArgs);
  bool containsUnboundTypeParam(const Type& type);

} // namespace noria::internal
