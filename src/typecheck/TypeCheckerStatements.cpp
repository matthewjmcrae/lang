#include "TypeCheckerInternal.hpp"

#include "noria/Builtins.hpp"
#include "noria/Constraints.hpp"
#include "noria/Diagnostic.hpp"
#include "noria/Monomorphize.hpp"
#include "noria/SemanticTables.hpp"

#include "TypeCheckerSupport.hpp"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace noria {

  using namespace typecheck_detail;

  StatementChecker::StatementChecker(TypeCheckContext& context, TypeRelations& relations,
                                     DeclarationChecker& declarations, ExpressionChecker& expressions,
                                     PlaceChecker& places, StructChecker& structs)
      : TypeCheckComponent(context), relations_(relations), declarations_(declarations),
        expressions_(expressions), places_(places), structs_(structs) {}

  StatementChecker::StatementVisitor::StatementVisitor(StatementChecker& state, Type expectedReturnType)
      : StatementOnlyVisitor("typecheck"), state_(state),
        expectedReturnType_(expectedReturnType) {}

  void StatementChecker::StatementVisitor::visit(const ast::LetStatement& letStatement) {
    const bool allowInternal = state_.isStdlibContext();
    std::optional<Type> declaredType = letStatement.declaredType;
    if (declaredType) {
      state_.requireKnownType(*declaredType, letStatement.location, nullptr, false,
                                allowInternal);
    }

    Type localType;
    if (letStatement.initializer) {
      const Type initializerType = state_.checkRvalue(*letStatement.initializer, declaredType);
      if (declaredType) {
        localType = *declaredType;
        if (!state_.isAssignable(localType, initializerType)) {
          throw CompileError(
              formatDiagnostic(letStatement.initializer->location, DiagnosticStage::TypeCheck,
                               "cannot initialize '" + letStatement.name + "' of type " +
                                   localType.name() + " with " + initializerType.name()));
        }
      } else {
        localType = initializerType;
        if (localType == Type::voidType()) {
          throw CompileError(formatDiagnostic(
              letStatement.initializer->location, DiagnosticStage::TypeCheck,
              "cannot infer local variable '" + letStatement.name + "' from void initializer"));
        }
        state_.requireKnownType(localType, letStatement.location, nullptr, false, allowInternal);
      }
    } else {
      if (!declaredType) {
        throw CompileError(formatDiagnostic(letStatement.location, DiagnosticStage::TypeCheck,
                                            "local declaration '" + letStatement.name +
                                                "' requires a type or initializer"));
      }
      localType = *declaredType;
      state_.requireDefaultInitializable(localType, letStatement.location);
    }

    if (localType == Type::voidType()) {
      throw CompileError(
          formatDiagnostic(letStatement.location, DiagnosticStage::TypeCheck,
                           "local variable '" + letStatement.name + "' cannot have type void"));
    }

    if (!state_.declareLocal(letStatement.name, localType)) {
      throw CompileError(formatDiagnostic(letStatement.location, DiagnosticStage::TypeCheck,
                                          "duplicate local variable '" + letStatement.name + "'"));
    }
    state_.requireContainerOwnershipOps(localType, letStatement.location);

    returned_ = false;
  }

  void StatementChecker::StatementVisitor::visit(const ast::AssignmentStatement& assignmentStatement) {
    const auto place = state_.checkPlace(*assignmentStatement.lhs);
    const Type valueType = state_.checkRvalue(*assignmentStatement.rhs, place.type);

    if (!state_.isAssignable(place.type, valueType)) {
      throw CompileError(formatDiagnostic(assignmentStatement.rhs->location,
                                          DiagnosticStage::TypeCheck,
                                          "cannot assign " + valueType.name() + " to variable '" +
                                              place.name + "' of type " + place.type.name()));
    }

    returned_ = false;
  }

  void StatementChecker::StatementVisitor::visit(const ast::ReturnStatement& returnStatement) {
    if (!returnStatement.expression) {
      if (expectedReturnType_ != Type::voidType()) {
        throw CompileError(formatDiagnostic(returnStatement.location, DiagnosticStage::TypeCheck,
                                            "non-void function must return a value"));
      }
      returned_ = true;
      return;
    }

    if (expectedReturnType_ == Type::voidType()) {
      throw CompileError(formatDiagnostic(returnStatement.location, DiagnosticStage::TypeCheck,
                                          "void function cannot return a value"));
    }

    const Type returnType = state_.checkRvalue(*returnStatement.expression, expectedReturnType_);

    if (!state_.isAssignable(expectedReturnType_, returnType)) {
      throw CompileError(
          formatDiagnostic(returnStatement.expression->location, DiagnosticStage::TypeCheck,
                           "return type " + returnType.name() + " does not match expected " +
                               expectedReturnType_.name()));
    }

    returned_ = true;
  }

  void StatementChecker::StatementVisitor::visit(const ast::IfStatement& ifStatement) {
    const Type conditionType = state_.checkRvalue(*ifStatement.condition);
    if (conditionType != Type::boolean()) {
      throw CompileError(
          formatDiagnostic(ifStatement.condition->location, DiagnosticStage::TypeCheck,
                           "if condition must be bool, got " + conditionType.name()));
    }

    const bool thenReturns = [&] {
      auto scope = state_.scopeFrame();
      return state_.checkStatements(ifStatement.thenBranch, expectedReturnType_);
    }();

    const bool elseReturns = [&] {
      auto scope = state_.scopeFrame();
      return state_.checkStatements(ifStatement.elseBranch, expectedReturnType_);
    }();

    returned_ = thenReturns && elseReturns;
  }

  void StatementChecker::StatementVisitor::visit(const ast::WhileStatement& whileStatement) {
    const Type conditionType = state_.checkRvalue(*whileStatement.condition);
    if (conditionType != Type::boolean()) {
      throw CompileError(
          formatDiagnostic(whileStatement.condition->location, DiagnosticStage::TypeCheck,
                           "while condition must be bool, got " + conditionType.name()));
    }

    auto scope = state_.scopeFrame();
    state_.checkStatements(whileStatement.body, expectedReturnType_);
    returned_ = false;
  }

  void StatementChecker::StatementVisitor::visit(const ast::ExpressionStatement& expressionStatement) {
    if (dynamic_cast<const ast::CallExpression*>(expressionStatement.expression.get()) == nullptr) {
      throw CompileError(formatDiagnostic(expressionStatement.location, DiagnosticStage::TypeCheck,
                                          "expression statement must be a function call"));
    }

    const Type expressionType = state_.checkRvalue(*expressionStatement.expression);
    if (expressionType != Type::voidType()) {
      throw CompileError(formatDiagnostic(expressionStatement.expression->location,
                                          DiagnosticStage::TypeCheck,
                                          "expression statement must call a void function"));
    }

    returned_ = false;
  }

} // namespace noria
