#include "noria/TypeChecker.hpp"

#include "TypeCheckerInternal.hpp"
#include "TypeCheckerStrategy.hpp"

#include <memory>
#include <concepts>
#include <stdexcept>
#include <utility>

namespace noria {

  static_assert(std::derived_from<TypeCheckerDriver, TypeCheckerStrategy>);
  static_assert(std::derived_from<TypeCheckerCalls, TypeCheckerStrategy>);
  static_assert(std::derived_from<TypeCheckerDeclarations, TypeCheckerStrategy>);
  static_assert(std::derived_from<TypeCheckerExpressions, TypeCheckerStrategy>);
  static_assert(std::derived_from<TypeCheckerPlaces, TypeCheckerStrategy>);
  static_assert(std::derived_from<TypeCheckerStatements, TypeCheckerStrategy>);
  static_assert(std::derived_from<TypeCheckerStructs, TypeCheckerStrategy>);
  static_assert(std::derived_from<TypeRelationsStrategy, TypeCheckerStrategy>);

  void TypeCheckerStrategy::check(TypeChecker&, const ast::Module&, const SymbolOrigins&) {
    throw std::logic_error("typecheck: invalid strategy endpoint");
  }
  void TypeCheckerStrategy::checkSpecializationFrontier(TypeChecker&, const ast::Module&, std::size_t,
                                                        std::size_t, const SymbolOrigins&) {
    throw std::logic_error("typecheck: invalid strategy endpoint");
  }
  void TypeCheckerDriver::check(TypeChecker& checker, const ast::Module& module,
                                const SymbolOrigins& origins) {
    checker.checkImpl(module, origins);
  }
  void TypeCheckerDriver::checkSpecializationFrontier(TypeChecker& checker, const ast::Module& module,
                                                       std::size_t firstStruct, std::size_t firstFunction,
                                                       const SymbolOrigins& origins) {
    checker.checkSpecializationFrontierImpl(module, firstStruct, firstFunction, origins);
  }
  std::unique_ptr<TypeCheckerStrategy> makeTypeCheckerStrategy(TypeCheckerStrategyKind kind) {
    switch (kind) {
    case TypeCheckerStrategyKind::Driver: return std::make_unique<TypeCheckerDriver>();
    case TypeCheckerStrategyKind::Calls: return std::make_unique<TypeCheckerCalls>();
    case TypeCheckerStrategyKind::Declarations: return std::make_unique<TypeCheckerDeclarations>();
    case TypeCheckerStrategyKind::Expressions: return std::make_unique<TypeCheckerExpressions>();
    case TypeCheckerStrategyKind::Places: return std::make_unique<TypeCheckerPlaces>();
    case TypeCheckerStrategyKind::Statements: return std::make_unique<TypeCheckerStatements>();
    case TypeCheckerStrategyKind::Structs: return std::make_unique<TypeCheckerStructs>();
    case TypeCheckerStrategyKind::Relations: return std::make_unique<TypeRelationsStrategy>();
    }
    throw std::logic_error("typecheck: unknown strategy");
  }

  TypeChecker makeTypeCheckerWithDriverStrategy() { return TypeChecker{}; }

  TypeChecker::TypeChecker()
      : relations_(*this),
        activeStrategy_(makeTypeCheckerStrategy(TypeCheckerStrategyKind::Driver)) {}
  TypeChecker::~TypeChecker() = default;
  TypeChecker::TypeChecker(TypeChecker&& other) noexcept
      : environment_(std::move(other.environment_)), session_(std::move(other.session_)),
        relations_(*this), activeStrategy_(std::move(other.activeStrategy_)) {
    if (!activeStrategy_) activeStrategy_ = makeTypeCheckerStrategy(TypeCheckerStrategyKind::Driver);
  }
  TypeChecker& TypeChecker::operator=(TypeChecker&& other) noexcept {
    if (this != &other) {
      environment_ = std::move(other.environment_);
      session_ = std::move(other.session_);
      activeStrategy_ = std::move(other.activeStrategy_);
      if (!activeStrategy_) activeStrategy_ = makeTypeCheckerStrategy(TypeCheckerStrategyKind::Driver);
    }
    return *this;
  }

  TypeChecker::StrategyScope::StrategyScope(TypeChecker& checker, TypeCheckerStrategyKind requested)
      : checker_(checker) {
    if (checker_.activeStrategy_->kind() != requested) {
      previous_ = std::move(checker_.activeStrategy_);
      checker_.activeStrategy_ = makeTypeCheckerStrategy(requested);
    }
  }
  TypeChecker::StrategyScope::~StrategyScope() {
    if (previous_) checker_.activeStrategy_ = std::move(previous_);
  }
  TypeChecker::StrategyScope TypeChecker::activate(TypeCheckerStrategyKind requested) {
    return StrategyScope(*this, requested);
  }

  void TypeChecker::check(const ast::Module& module, const SymbolOrigins& origins) {
    activeStrategy_->check(*this, module, origins);
  }
  void TypeChecker::checkSpecializationFrontier(const ast::Module& module, std::size_t firstStruct,
                                                std::size_t firstFunction, const SymbolOrigins& origins) {
    activeStrategy_->checkSpecializationFrontier(*this, module, firstStruct, firstFunction, origins);
  }
  void TypeChecker::registerFunctionSpecialization(std::string mangledName, std::vector<Type> typeArgs) {
    session_.functionSpecializationTypeArgs.emplace(std::move(mangledName), std::move(typeArgs));
  }
  const std::vector<SpecializationRequest>& TypeChecker::specializationRequests() const { return session_.specializationRequests; }
  const std::vector<StructSpecializationRequest>& TypeChecker::structSpecializationRequests() const { return session_.structSpecializationRequests; }
  void TypeChecker::clearSpecializationRequests() { session_.specializationRequests.clear(); }
  void TypeChecker::clearStructSpecializationRequests() { session_.structSpecializationRequests.clear(); }
  std::vector<SpecializationRequest> TypeChecker::takeSpecializationRequests() {
    std::vector<SpecializationRequest> requests;
    requests.swap(session_.specializationRequests);
    return requests;
  }
  std::vector<StructSpecializationRequest> TypeChecker::takeStructSpecializationRequests() const {
    std::vector<StructSpecializationRequest> requests;
    requests.swap(session_.structSpecializationRequests);
    return requests;
  }

  void TypeChecker::requireKnownType(const Type& type, SourceLocation location,
                                     const std::unordered_set<std::string>* params,
                                     bool allowImplTags, bool allowInternalTypes) const {
    auto strategy = const_cast<TypeChecker*>(this)->activate(TypeCheckerStrategyKind::Relations);
    relations_.requireKnownType(type, location, params, allowImplTags, allowInternalTypes);
  }
  void TypeChecker::unifyTypes(const Type& expected, const Type& actual,
                               std::unordered_map<std::string, Type>& bindings,
                               SourceLocation location) const {
    auto strategy = const_cast<TypeChecker*>(this)->activate(TypeCheckerStrategyKind::Relations);
    relations_.unifyTypes(expected, actual, bindings, location);
  }
  bool TypeChecker::isAssignable(Type expected, Type actual) const {
    auto strategy = const_cast<TypeChecker*>(this)->activate(TypeCheckerStrategyKind::Relations);
    return relations_.isAssignable(std::move(expected), std::move(actual));
  }
  void TypeChecker::checkSpecializationConstraints(const std::string& name,
                                                   const std::vector<Type>& args,
                                                   SourceLocation location) const {
    auto strategy = const_cast<TypeChecker*>(this)->activate(TypeCheckerStrategyKind::Relations);
    relations_.checkSpecializationConstraints(name, args, location);
  }
  void TypeChecker::recordStructSpecialization(const std::string& name, const std::vector<Type>& args,
                                               SourceLocation location) const {
    auto strategy = const_cast<TypeChecker*>(this)->activate(TypeCheckerStrategyKind::Relations);
    relations_.recordStructSpecialization(name, args, location);
  }

} // namespace noria
