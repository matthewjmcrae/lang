#pragma once

#include "noria/Ast.hpp"
#include "noria/ModuleResolver.hpp"
#include "noria/Monomorphize.hpp"
#include "noria/Types.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace noria {

  struct FunctionSignature {
    Type returnType;
    std::vector<Type> parameterTypes;
  };

  class TypeChecker {
  public:
    TypeChecker();
    ~TypeChecker();
    TypeChecker(const TypeChecker& other);
    TypeChecker& operator=(const TypeChecker& other);
    TypeChecker(TypeChecker&& other) noexcept;
    TypeChecker& operator=(TypeChecker&& other) noexcept;

    void check(const ast::Module& module, const SymbolOrigins& symbolOrigins = {});
    void checkSpecializationFrontier(const ast::Module& module, std::size_t firstNewStruct,
                                     std::size_t firstNewFunction,
                                     const SymbolOrigins& symbolOrigins = {});

    void registerFunctionSpecialization(std::string mangledName, std::vector<Type> typeArgs);

    const std::vector<SpecializationRequest>& specializationRequests() const;
    const std::vector<StructSpecializationRequest>& structSpecializationRequests() const;
    void clearSpecializationRequests();
    void clearStructSpecializationRequests();
    std::vector<SpecializationRequest> takeSpecializationRequests();
    std::vector<StructSpecializationRequest> takeStructSpecializationRequests() const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
  };

} // namespace noria
