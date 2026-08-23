#pragma once

#include "noria/Ast.hpp"
#include "noria/Types.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace noria {

  using Substitution = std::unordered_map<std::string, Type>;

  struct SpecializationRequest {
    std::string templateName;
    std::vector<Type> typeArgs;
    SourceLocation callSiteLocation;
    std::string enclosingFunction;
  };

  Type substitute(const Type& type, const Substitution& substitution);

  std::string mangleType(const Type& type);
  std::string mangleSpecialization(std::string_view templateName,
                                   const std::vector<Type>& typeArgs);

  std::size_t expandSpecializations(ast::Module& module,
                                    const std::vector<SpecializationRequest>& requests);

  void rewriteGenericCallSites(ast::Module& module,
                               const std::vector<SpecializationRequest>& requests);

} // namespace noria
