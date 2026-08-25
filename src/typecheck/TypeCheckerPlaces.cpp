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

  namespace {

    [[noreturn]] void invalidAssignmentTarget(SourceLocation location) {
      throw CompileError(
          formatDiagnostic(location, DiagnosticStage::TypeCheck, "invalid assignment target"));
    }

    bool isFieldAssignmentPlaceBase(const ast::Expression& expression) {
      if (dynamic_cast<const ast::IdentifierExpression*>(&expression) != nullptr) {
        return true;
      }

      if (const auto* index = dynamic_cast<const ast::IndexExpression*>(&expression)) {
        const ast::Expression* root = index->base.get();
        while (const auto* nested = dynamic_cast<const ast::IndexExpression*>(root)) {
          root = nested->base.get();
        }
        return dynamic_cast<const ast::IdentifierExpression*>(root) != nullptr;
      }

      if (const auto* fieldAccess = dynamic_cast<const ast::FieldAccessExpression*>(&expression)) {
        return isFieldAssignmentPlaceBase(*fieldAccess->base);
      }

      return false;
    }

    std::string fieldAssignmentRootName(const ast::Expression& expression) {
      const ast::Expression* current = &expression;
      while (const auto* index = dynamic_cast<const ast::IndexExpression*>(current)) {
        current = index->base.get();
      }
      while (const auto* fieldAccess = dynamic_cast<const ast::FieldAccessExpression*>(current)) {
        current = fieldAccess->base.get();
      }

      if (const auto* identifier = dynamic_cast<const ast::IdentifierExpression*>(current)) {
        return identifier->name;
      }

      throw CompileError("typecheck: internal error: missing field assignment root identifier");
    }

  } // namespace

  TypeChecker::Impl::PlaceVisitor::PlaceVisitor(TypeChecker::Impl& checker) : checker_(checker) {}

  void TypeChecker::Impl::PlaceVisitor::visit(const ast::IdentifierExpression& identifier) {
    name_ = identifier.name;
    type_ = checker_.lookupLocal(identifier.name, identifier.location);
  }

  void TypeChecker::Impl::PlaceVisitor::visit(const ast::IntegerLiteral& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::Impl::PlaceVisitor::visit(const ast::FloatLiteral& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::Impl::PlaceVisitor::visit(const ast::StringLiteral& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::Impl::PlaceVisitor::visit(const ast::BoolLiteral& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::Impl::PlaceVisitor::visit(const ast::UnaryExpression& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::Impl::PlaceVisitor::visit(const ast::CastExpression& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::Impl::PlaceVisitor::visit(const ast::BinaryExpression& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::Impl::PlaceVisitor::visit(const ast::CallExpression& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::Impl::PlaceVisitor::visit(const ast::ArrayLiteral& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::Impl::PlaceVisitor::visit(const ast::StructLiteral& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::Impl::PlaceVisitor::visit(const ast::IndexExpression& index) {
    const Type baseType = checker_.checkRvalue(*index.base);
    const Type indexType = checker_.checkRvalue(*index.index);

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

      const ast::Expression* root = index.base.get();
      while (const auto* nested = dynamic_cast<const ast::IndexExpression*>(root)) {
        root = nested->base.get();
      }
      if (const auto* identifier = dynamic_cast<const ast::IdentifierExpression*>(root)) {
        name_ = identifier->name;
        return;
      }
      invalidAssignmentTarget(index.location);
    }

    throw CompileError(
        formatDiagnostic(index.base->location, DiagnosticStage::TypeCheck,
                         "index requires str or array base, got " + baseType.name()));
  }

  void TypeChecker::Impl::PlaceVisitor::visit(const ast::FieldAccessExpression& access) {
    const Type baseType = checker_.checkRvalue(*access.base);
    if (baseType.kind != TypeKind::Struct) {
      throw CompileError(
          formatDiagnostic(access.base->location, DiagnosticStage::TypeCheck,
                           "field access requires struct base, got " + baseType.name()));
    }

    const StructInfo structInfo = checker_.resolveStructInfo(baseType, access.location);
    const auto fieldIndex = structInfo.fieldIndex.find(access.fieldName);
    if (fieldIndex == structInfo.fieldIndex.end()) {
      throw CompileError(formatDiagnostic(access.location, DiagnosticStage::TypeCheck,
                                          "struct '" + baseType.structName + "' has no field '" +
                                              access.fieldName + "'"));
    }

    const StructFieldInfo& fieldInfo = structInfo.fields[fieldIndex->second];
    checker_.requireFieldVisible(baseType.structName, fieldInfo, access.location);

    if (!isFieldAssignmentPlaceBase(*access.base)) {
      invalidAssignmentTarget(access.location);
    }

    name_ = fieldAssignmentRootName(*access.base) + "." + access.fieldName;
    type_ = fieldInfo.type;
  }

  void TypeChecker::Impl::PlaceVisitor::visit(const ast::ReturnStatement& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::Impl::PlaceVisitor::visit(const ast::LetStatement& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::Impl::PlaceVisitor::visit(const ast::IfStatement& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::Impl::PlaceVisitor::visit(const ast::WhileStatement& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::Impl::PlaceVisitor::visit(const ast::AssignmentStatement& node) {
    invalidAssignmentTarget(node.location);
  }
  void TypeChecker::Impl::PlaceVisitor::visit(const ast::ExpressionStatement& node) {
    invalidAssignmentTarget(node.location);
  }

  TypeChecker::Impl::PlaceInfo TypeChecker::Impl::checkPlace(const ast::Expression& place) {
    PlaceVisitor visitor(*this);
    place.accept(visitor);
    return PlaceInfo{visitor.name(), visitor.type()};
  }

} // namespace noria
