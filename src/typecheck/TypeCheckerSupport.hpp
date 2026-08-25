#pragma once

#include "../internal/SpecializationSupport.hpp"
#include "noria/Ast.hpp"
#include "noria/Monomorphize.hpp"

#include <optional>
#include <string_view>
#include <vector>

namespace noria::typecheck_detail {

  bool isScalarWitnessType(const Type& type);
  using internal::containsUnboundTypeParam;
  using internal::findImplTag;
  using internal::firstNonImplTagTypeArg;

  bool structSpecializationsMatch(const Type& left, const Type& right);
  bool allTypeParamsSubstituted(const Type& type, const Substitution& substitution);
  bool sameGenericPublicApi(const ast::Function& left, const ast::Function& right);
  void rejectStructArrayElement(const Type& elementType, SourceLocation location);
  const ast::Function* selectGenericImplementation(const ast::Module& module,
                                                   const std::vector<std::size_t>& family,
                                                   std::optional<ImplementationTag> callTag,
                                                   std::string_view functionName,
                                                   SourceLocation location);
} // namespace noria::typecheck_detail
