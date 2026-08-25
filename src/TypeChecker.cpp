#include "noria/TypeChecker.hpp"

#include "typecheck/TypeCheckerInternal.hpp"

#include <memory>
#include <utility>

namespace noria {

  TypeChecker::TypeChecker() : impl_(std::make_unique<Impl>()) {}
  TypeChecker::~TypeChecker() = default;

  TypeChecker::TypeChecker(const TypeChecker& other)
      : impl_(std::make_unique<Impl>(*other.impl_)) {}

  TypeChecker& TypeChecker::operator=(const TypeChecker& other) {
    if (this != &other) {
      impl_ = std::make_unique<Impl>(*other.impl_);
    }
    return *this;
  }

  TypeChecker::TypeChecker(TypeChecker&& other) noexcept = default;
  TypeChecker& TypeChecker::operator=(TypeChecker&& other) noexcept = default;

  void TypeChecker::check(const ast::Module& module, const SymbolOrigins& symbolOrigins) {
    impl_->check(module, symbolOrigins);
  }

  void TypeChecker::checkSpecializationFrontier(const ast::Module& module,
                                                std::size_t firstNewStruct,
                                                std::size_t firstNewFunction,
                                                const SymbolOrigins& symbolOrigins) {
    impl_->checkSpecializationFrontier(module, firstNewStruct, firstNewFunction, symbolOrigins);
  }

  void TypeChecker::registerFunctionSpecialization(std::string mangledName,
                                                   std::vector<Type> typeArgs) {
    impl_->registerFunctionSpecialization(std::move(mangledName), std::move(typeArgs));
  }

  const std::vector<SpecializationRequest>& TypeChecker::specializationRequests() const {
    return impl_->specializationRequests();
  }

  const std::vector<StructSpecializationRequest>&
  TypeChecker::structSpecializationRequests() const {
    return impl_->structSpecializationRequests();
  }

  void TypeChecker::clearSpecializationRequests() {
    impl_->clearSpecializationRequests();
  }

  void TypeChecker::clearStructSpecializationRequests() {
    impl_->clearStructSpecializationRequests();
  }

  std::vector<SpecializationRequest> TypeChecker::takeSpecializationRequests() {
    return impl_->takeSpecializationRequests();
  }

  std::vector<StructSpecializationRequest> TypeChecker::takeStructSpecializationRequests() const {
    return impl_->takeStructSpecializationRequests();
  }

} // namespace noria
