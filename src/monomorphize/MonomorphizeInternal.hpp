#pragma once

#include "noria/Monomorphize.hpp"

#include <optional>
#include <string_view>

namespace noria::monomorphize_detail {

  struct PendingSpecializations {
    std::vector<StructSpecializationRequest> structs;
    std::vector<SpecializationRequest> functions;

    bool empty() const { return structs.empty() && functions.empty(); }
  };

  Type substituteType(const Type& type, const Substitution& substitution);
  Type rewriteAppliedStructType(const Type& type);
  Substitution bindTypeParameters(const std::vector<ast::TypeParameter>& typeParams,
                                  const std::vector<Type>& typeArgs, std::string_view context);

  const ast::Function* findTemplateFunction(const ast::Module& module, std::string_view name,
                                            std::optional<ImplementationTag> implTag);
  const ast::StructDecl* findTemplateStruct(const ast::Module& module, std::string_view name);
  ast::Function cloneSpecialization(const ast::Function& templated,
                                    const std::vector<Type>& typeArgs);
  ast::StructDecl cloneStructSpecialization(const ast::StructDecl& templated,
                                            const std::vector<Type>& typeArgs);

  std::vector<SpecializationRequest>
  sortedFunctionRequests(const std::vector<SpecializationRequest>& requests);
  std::vector<StructSpecializationRequest>
  sortedStructRequests(const std::vector<StructSpecializationRequest>& requests);
  std::size_t
  emitUniqueFunctionSpecializations(ast::Module& module,
                                    const std::vector<SpecializationRequest>& sortedRequests,
                                    SpecializationCache& cache);
  std::size_t
  emitUniqueStructSpecializations(ast::Module& module,
                                  const std::vector<StructSpecializationRequest>& sortedRequests,
                                  SpecializationCache& cache);

  void rewriteTargetedGenericCallSites(ast::Module& module,
                                       const std::vector<SpecializationRequest>& requests);
  void rewriteStructApplications(ast::Module& module,
                                 const std::vector<StructSpecializationRequest>& requests);

} // namespace noria::monomorphize_detail
