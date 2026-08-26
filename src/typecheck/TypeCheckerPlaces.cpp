#include "TypeCheckerInternal.hpp"
#include "TypeCheckerState.hpp"

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

  namespace {

    [[noreturn]] void invalidAssignmentTarget(SourceLocation location) {
      throw CompileError(
          formatDiagnostic(location, DiagnosticStage::TypeCheck, "invalid assignment target"));
    }

    const ast::IdentifierExpression* assignmentPlaceRoot(const ast::Expression& expression) {
      const ast::Expression* current = &expression;
      while (true) {
        if (const auto* index = dynamic_cast<const ast::IndexExpression*>(current)) {
          current = index->base.get();
          continue;
        }
        if (const auto* fieldAccess = dynamic_cast<const ast::FieldAccessExpression*>(current)) {
          current = fieldAccess->base.get();
          continue;
        }
        break;
      }
      return dynamic_cast<const ast::IdentifierExpression*>(current);
    }

  } // namespace

  TypeChecker::PlacesState::PlaceVisitor::PlaceVisitor(PlacesState& state) : state_(state) {}

  void TypeChecker::PlacesState::PlaceVisitor::visit(const ast::IdentifierExpression& identifier) {
    name_ = identifier.name;
    type_ = state_.lookupLocal(identifier.name, identifier.location);
  }

  void TypeChecker::PlacesState::PlaceVisitor::visit(const ast::IntegerLiteral& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::PlacesState::PlaceVisitor::visit(const ast::FloatLiteral& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::PlacesState::PlaceVisitor::visit(const ast::StringLiteral& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::PlacesState::PlaceVisitor::visit(const ast::BoolLiteral& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::PlacesState::PlaceVisitor::visit(const ast::UnaryExpression& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::PlacesState::PlaceVisitor::visit(const ast::CastExpression& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::PlacesState::PlaceVisitor::visit(const ast::BinaryExpression& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::PlacesState::PlaceVisitor::visit(const ast::CallExpression& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::PlacesState::PlaceVisitor::visit(const ast::ArrayLiteral& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::PlacesState::PlaceVisitor::visit(const ast::StructLiteral& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::PlacesState::PlaceVisitor::visit(const ast::IndexExpression& index) {
    const Type baseType = state_.checkRvalue(*index.base);
    const Type indexType = state_.checkRvalue(*index.index);

    if (baseType == Type::str()) {
      throw CompileError(formatDiagnostic(index.location, DiagnosticStage::TypeCheck,
                                          "str index is not assignable"));
    }

    if (baseType.kind == TypeKind::Array) {
      if (!baseType.element) {
        throw CompileError(
            formatDiagnostic(index.base->location, DiagnosticStage::TypeCheck,
                             "index requires str or array base, got " + baseType.name()));
      }
      if (indexType != Type::i32()) {
        throw CompileError(formatDiagnostic(index.index->location, DiagnosticStage::TypeCheck,
                                            "index requires i32 index, got " + indexType.name()));
      }
      type_ = *baseType.element;

      if (const auto* identifier = assignmentPlaceRoot(index)) {
        name_ = identifier->name;
        return;
      }
      invalidAssignmentTarget(index.location);
    }

    const Type canonical = state_.canonicalStructType(baseType);
    if (const std::optional<StandardContainer> container =
            state_.standardContainerFor(canonical)) {
      const auto requireIndexType = [&](const Type& expected, std::string_view name) {
        if (!state_.isAssignable(expected, indexType)) {
          throw CompileError(formatDiagnostic(index.index->location, DiagnosticStage::TypeCheck,
                                              std::string(name) + " index expects " +
                                                  expected.name() + ", got " + indexType.name()));
        }
      };

      if (*container == StandardContainer::Set) {
        throw CompileError(formatDiagnostic(index.location, DiagnosticStage::TypeCheck,
                                            "set index is not assignable"));
      }

      if (*container == StandardContainer::Sequence) {
        requireIndexType(Type::i32(), "sequence");
        state_.recordImplicitContainerOperation(*container, ContainerOperation::Set,
                                                  canonical.typeArgs, index.location);
        type_ = canonical.typeArgs[0];
      } else {
        requireIndexType(canonical.typeArgs[0], "dictionary");
        state_.recordImplicitContainerOperation(*container, ContainerOperation::Insert,
                                                  canonical.typeArgs, index.location);
        type_ = canonical.typeArgs[1];
      }

      index.standardContainer = std::make_pair(*container, canonical);
      if (const auto* identifier = assignmentPlaceRoot(index)) {
        name_ = identifier->name;
        return;
      }
      invalidAssignmentTarget(index.location);
    }

    throw CompileError(
        formatDiagnostic(index.base->location, DiagnosticStage::TypeCheck,
                         "index requires str, array, Sequence, Dictionary, or Set base, got " +
                             baseType.name()));
  }

  void TypeChecker::PlacesState::PlaceVisitor::visit(const ast::FieldAccessExpression& access) {
    const Type baseType = state_.checkRvalue(*access.base);
    if (baseType.kind != TypeKind::Struct) {
      throw CompileError(
          formatDiagnostic(access.base->location, DiagnosticStage::TypeCheck,
                           "field access requires struct base, got " + baseType.name()));
    }

    const StructInfo structInfo = state_.resolveStructInfo(baseType, access.location);
    const auto fieldIndex = structInfo.fieldIndex.find(access.fieldName);
    if (fieldIndex == structInfo.fieldIndex.end()) {
      throw CompileError(formatDiagnostic(access.location, DiagnosticStage::TypeCheck,
                                          "struct '" + baseType.structName + "' has no field '" +
                                              access.fieldName + "'"));
    }

    const StructFieldInfo& fieldInfo = structInfo.fields[fieldIndex->second];
    state_.requireFieldVisible(baseType.structName, fieldInfo, access.location);

    if (const auto* identifier = assignmentPlaceRoot(*access.base)) {
      name_ = identifier->name + "." + access.fieldName;
      type_ = fieldInfo.type;
      return;
    }
    invalidAssignmentTarget(access.location);
  }

  void TypeChecker::PlacesState::PlaceVisitor::visit(const ast::ReturnStatement& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::PlacesState::PlaceVisitor::visit(const ast::LetStatement& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::PlacesState::PlaceVisitor::visit(const ast::IfStatement& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::PlacesState::PlaceVisitor::visit(const ast::WhileStatement& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::PlacesState::PlaceVisitor::visit(const ast::AssignmentStatement& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::PlacesState::PlaceVisitor::visit(const ast::ExpressionStatement& node) {
    invalidAssignmentTarget(node.location);
  }

  TypeChecker::PlaceInfo TypeChecker::PlacesState::checkPlace(const ast::Expression& place) {
    PlaceVisitor visitor(*this);
    place.accept(visitor);
    return PlaceInfo{visitor.name(), visitor.type()};
  }

} // namespace noria
