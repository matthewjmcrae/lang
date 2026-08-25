#pragma once

#include "noria/TypeChecker.hpp"

#include <memory>

namespace noria {

  enum class TypeCheckerStrategyKind {
    Driver, Calls, Declarations, Expressions, Places, Statements, Structs, Relations
  };

  class TypeCheckerStrategy {
  public:
    virtual ~TypeCheckerStrategy() = default;
    virtual TypeCheckerStrategyKind kind() const noexcept = 0;
    virtual void check(TypeChecker&, const ast::Module&, const SymbolOrigins&);
    virtual void checkSpecializationFrontier(TypeChecker&, const ast::Module&, std::size_t,
                                             std::size_t, const SymbolOrigins&);
  };

  class TypeCheckerDriver final : public TypeCheckerStrategy {
  public:
    TypeCheckerStrategyKind kind() const noexcept override { return TypeCheckerStrategyKind::Driver; }
    void check(TypeChecker&, const ast::Module&, const SymbolOrigins&) override;
    void checkSpecializationFrontier(TypeChecker&, const ast::Module&, std::size_t, std::size_t,
                                     const SymbolOrigins&) override;
  };
  class TypeCheckerCalls final : public TypeCheckerStrategy { public: TypeCheckerStrategyKind kind() const noexcept override { return TypeCheckerStrategyKind::Calls; } };
  class TypeCheckerDeclarations final : public TypeCheckerStrategy { public: TypeCheckerStrategyKind kind() const noexcept override { return TypeCheckerStrategyKind::Declarations; } };
  class TypeCheckerExpressions final : public TypeCheckerStrategy { public: TypeCheckerStrategyKind kind() const noexcept override { return TypeCheckerStrategyKind::Expressions; } };
  class TypeCheckerPlaces final : public TypeCheckerStrategy { public: TypeCheckerStrategyKind kind() const noexcept override { return TypeCheckerStrategyKind::Places; } };
  class TypeCheckerStatements final : public TypeCheckerStrategy { public: TypeCheckerStrategyKind kind() const noexcept override { return TypeCheckerStrategyKind::Statements; } };
  class TypeCheckerStructs final : public TypeCheckerStrategy { public: TypeCheckerStrategyKind kind() const noexcept override { return TypeCheckerStrategyKind::Structs; } };
  class TypeRelationsStrategy final : public TypeCheckerStrategy { public: TypeCheckerStrategyKind kind() const noexcept override { return TypeCheckerStrategyKind::Relations; } };
  std::unique_ptr<TypeCheckerStrategy> makeTypeCheckerStrategy(TypeCheckerStrategyKind);
  TypeChecker makeTypeCheckerWithDriverStrategy();

} // namespace noria
