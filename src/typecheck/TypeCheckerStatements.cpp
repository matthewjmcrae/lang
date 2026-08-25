#include "TypeCheckerInternal.hpp"
#include "TypeCheckerStrategy.hpp"

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

  TypeChecker::StatementVisitor::StatementVisitor(TypeChecker& checker,
                                                        Type expectedReturnType)
      : StatementOnlyVisitor("typecheck"), checker_(checker),
        expectedReturnType_(expectedReturnType) {}

  void TypeChecker::StatementVisitor::visit(const ast::LetStatement& letStatement) {
    const bool allowInternal = checker_.isStdlibContext();
    std::optional<Type> declaredType = letStatement.declaredType;
    if (declaredType) {
      checker_.requireKnownType(*declaredType, letStatement.location, nullptr, false,
                                allowInternal);
    }

    Type localType;
    if (letStatement.initializer) {
      const Type initializerType = checker_.checkRvalue(*letStatement.initializer, declaredType);
      if (declaredType) {
        localType = *declaredType;
        if (!checker_.isAssignable(localType, initializerType)) {
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
        checker_.requireKnownType(localType, letStatement.location, nullptr, false, allowInternal);
      }
    } else {
      if (!declaredType) {
        throw CompileError(formatDiagnostic(letStatement.location, DiagnosticStage::TypeCheck,
                                            "local declaration '" + letStatement.name +
                                                "' requires a type or initializer"));
      }
      localType = *declaredType;
    }

    if (localType == Type::voidType()) {
      throw CompileError(
          formatDiagnostic(letStatement.location, DiagnosticStage::TypeCheck,
                           "local variable '" + letStatement.name + "' cannot have type void"));
    }

    if (!checker_.declareLocal(letStatement.name, localType)) {
      throw CompileError(formatDiagnostic(letStatement.location, DiagnosticStage::TypeCheck,
                                          "duplicate local variable '" + letStatement.name + "'"));
    }

    returned_ = false;
  }

  void
  TypeChecker::StatementVisitor::visit(const ast::AssignmentStatement& assignmentStatement) {
    const auto place = checker_.checkPlace(*assignmentStatement.lhs);
    const Type valueType = checker_.checkRvalue(*assignmentStatement.rhs);

    if (!checker_.isAssignable(place.type, valueType)) {
      throw CompileError(formatDiagnostic(assignmentStatement.rhs->location,
                                          DiagnosticStage::TypeCheck,
                                          "cannot assign " + valueType.name() + " to variable '" +
                                              place.name + "' of type " + place.type.name()));
    }

    returned_ = false;
  }

  void TypeChecker::StatementVisitor::visit(const ast::ReturnStatement& returnStatement) {
    const Type returnType = checker_.checkRvalue(*returnStatement.expression, expectedReturnType_);

    if (!checker_.isAssignable(expectedReturnType_, returnType)) {
      throw CompileError(
          formatDiagnostic(returnStatement.expression->location, DiagnosticStage::TypeCheck,
                           "return type " + returnType.name() + " does not match expected " +
                               expectedReturnType_.name()));
    }

    returned_ = true;
  }

  void TypeChecker::StatementVisitor::visit(const ast::IfStatement& ifStatement) {
    const Type conditionType = checker_.checkRvalue(*ifStatement.condition);
    if (conditionType != Type::boolean()) {
      throw CompileError(
          formatDiagnostic(ifStatement.condition->location, DiagnosticStage::TypeCheck,
                           "if condition must be bool, got " + conditionType.name()));
    }

    checker_.pushScope();
    const bool thenReturns = checker_.checkStatements(ifStatement.thenBranch, expectedReturnType_);
    checker_.popScope();

    checker_.pushScope();
    const bool elseReturns = checker_.checkStatements(ifStatement.elseBranch, expectedReturnType_);
    checker_.popScope();

    returned_ = thenReturns && elseReturns;
  }

  void TypeChecker::StatementVisitor::visit(const ast::WhileStatement& whileStatement) {
    const Type conditionType = checker_.checkRvalue(*whileStatement.condition);
    if (conditionType != Type::boolean()) {
      throw CompileError(
          formatDiagnostic(whileStatement.condition->location, DiagnosticStage::TypeCheck,
                           "while condition must be bool, got " + conditionType.name()));
    }

    checker_.pushScope();
    checker_.checkStatements(whileStatement.body, expectedReturnType_);
    checker_.popScope();
    returned_ = false;
  }

  void
  TypeChecker::StatementVisitor::visit(const ast::ExpressionStatement& expressionStatement) {
    if (dynamic_cast<const ast::CallExpression*>(expressionStatement.expression.get()) == nullptr) {
      throw CompileError(formatDiagnostic(expressionStatement.location, DiagnosticStage::TypeCheck,
                                          "expression statement must be a function call"));
    }

    const Type expressionType = checker_.checkRvalue(*expressionStatement.expression);
    if (expressionType != Type::voidType()) {
      throw CompileError(formatDiagnostic(expressionStatement.expression->location,
                                          DiagnosticStage::TypeCheck,
                                          "expression statement must call a void builtin"));
    }

    returned_ = false;
  }

} // namespace noria
