#include "noria/TypeChecker.hpp"

#include "TypeCheckerInternal.hpp"

#include <memory>
#include <utility>

namespace noria {

  TypeChecker::TypeChecker() : impl_(std::make_unique<Impl>()) {}
  TypeChecker::~TypeChecker() = default;
  TypeChecker::TypeChecker(TypeChecker&&) noexcept = default;
  TypeChecker& TypeChecker::operator=(TypeChecker&&) noexcept = default;

  void TypeChecker::check(ast::Module& module, const SymbolOrigins& origins) {
    impl_->driver.check(module, origins);
  }

  void TypeChecker::checkSpecializationFrontier(const ast::Module& module, std::size_t firstStruct,
                                                std::size_t firstFunction,
                                                const SymbolOrigins& origins) {
    impl_->driver.checkSpecializationFrontier(module, firstStruct, firstFunction, origins);
  }

  void TypeChecker::registerFunctionSpecialization(std::string mangledName,
                                                   std::vector<Type> typeArgs) {
    impl_->context.specializations.registerFunction(std::move(mangledName), std::move(typeArgs));
  }

  void TypeChecker::registerStructSpecialization(std::string mangledName,
                                                 std::vector<Type> typeArgs) {
    impl_->context.specializations.registerStruct(std::move(mangledName), std::move(typeArgs));
  }

  const std::vector<SpecializationRequest>& TypeChecker::specializationRequests() const {
    return impl_->context.specializations.functionRequests();
  }

  const std::vector<StructSpecializationRequest>& TypeChecker::structSpecializationRequests() const {
    return impl_->context.specializations.structRequests();
  }

  void TypeChecker::clearSpecializationRequests() {
    impl_->context.specializations.clearFunctionRequests();
  }

  void TypeChecker::clearStructSpecializationRequests() {
    impl_->context.specializations.clearStructRequests();
  }

  std::vector<SpecializationRequest> TypeChecker::takeSpecializationRequests() {
    return impl_->context.specializations.takeFunctionRequests();
  }

  std::vector<StructSpecializationRequest> TypeChecker::takeStructSpecializationRequests() const {
    return impl_->context.specializations.takeStructRequests();
  }

} // namespace noria
