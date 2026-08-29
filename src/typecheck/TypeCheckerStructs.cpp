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

  StructChecker::StructChecker(TypeCheckContext& context, TypeRelations& relations,
                               DeclarationChecker& declarations)
      : TypeCheckComponent(context), relations_(relations), declarations_(declarations) {}

  void ExpressionChecker::ExpressionVisitor::visit(const ast::StructLiteral& literal) {
    result_ = state_.structs_.checkStructLiteral(state_, literal);
  }

  Type StructChecker::checkStructLiteral(ExpressionChecker& expressions,
                                         const ast::StructLiteral& literal) {
    const auto genericStruct = environment().genericStructs.find(literal.structName);
    if (genericStruct != environment().genericStructs.end()) {
      return checkGenericStructLiteral(expressions, literal,
                                       declarations_.genericStructAt(genericStruct->second));
    }

    if (!literal.typeArgs.empty()) {
      throw CompileError(formatDiagnostic(literal.location, DiagnosticStage::TypeCheck,
                                          "type '" + literal.structName +
                                              "' is not generic and cannot take type arguments"));
    }

    const StructInfo& structInfo = lookupStruct(literal.structName, literal.location);
    return checkConcreteStructLiteral(expressions, literal, structInfo, {});
  }

  Type StructChecker::checkGenericStructLiteral(ExpressionChecker& expressions,
                                                const ast::StructLiteral& literal,
                                                const ast::StructDecl& templated) {
    std::vector<Type> typeArgs = literal.typeArgs;
    if (typeArgs.empty()) {
      typeArgs = inferStructLiteralTypeArgs(expressions, literal, templated);
    } else if (typeArgs.size() != templated.typeParams.size()) {
      std::ostringstream out;
      out << "struct '" << literal.structName << "' expects " << templated.typeParams.size()
          << " type argument(s), got " << typeArgs.size();
      throw CompileError(formatDiagnostic(literal.location, DiagnosticStage::TypeCheck, out.str()));
    }

    for (const Type& typeArg : typeArgs) {
      relations_.requireKnownType(typeArg, literal.location, nullptr, true);
    }

    relations_.recordStructSpecialization(literal.structName, typeArgs, literal.location);
    const StructInfo structInfo =
        resolveStructInfo(Type::structType(literal.structName, typeArgs), literal.location);
    return checkConcreteStructLiteral(expressions, literal, structInfo, typeArgs);
  }

  std::vector<Type> StructChecker::inferStructLiteralTypeArgs(ExpressionChecker& expressions,
                                                               const ast::StructLiteral& literal,
                                                               const ast::StructDecl& templated) {
    std::unordered_map<std::string, Type> bindings;
    std::unordered_map<std::string, Type> provided;

    // Infer generic struct arguments from provided field values before resolving concrete fields.
    for (const auto& field : literal.fields) {
      if (provided.contains(field.name)) {
        throw CompileError(formatDiagnostic(field.location, DiagnosticStage::TypeCheck,
                                            "duplicate field '" + field.name +
                                                "' in struct literal for '" + literal.structName +
                                                "'"));
      }

      const auto templateField = std::find_if(
          templated.fields.begin(), templated.fields.end(),
          [&](const ast::StructField& candidate) { return candidate.name == field.name; });
      if (templateField == templated.fields.end()) {
        throw CompileError(formatDiagnostic(field.location, DiagnosticStage::TypeCheck,
                                            "struct '" + literal.structName + "' has no field '" +
                                                field.name + "'"));
      }

      requireFieldVisible(
          literal.structName,
          StructFieldInfo{templateField->name, templateField->type, 0, templateField->visibility},
          field.location);

      const Type actual = expressions.checkRvalue(*field.value);
      relations_.unifyTypes(templateField->type, actual, bindings, field.location);
      provided.emplace(field.name, actual);
    }

    for (const auto& expectedField : templated.fields) {
      if (!provided.contains(expectedField.name)) {
        throw CompileError(formatDiagnostic(literal.location, DiagnosticStage::TypeCheck,
                                            "struct literal for '" + literal.structName +
                                                "' is missing field '" + expectedField.name + "'"));
      }
    }

    std::vector<Type> typeArgs;
    typeArgs.reserve(templated.typeParams.size());
    for (const auto& typeParam : templated.typeParams) {
      const auto bound = bindings.find(typeParam.name);
      if (bound == bindings.end()) {
        throw CompileError(
            formatDiagnostic(literal.location, DiagnosticStage::TypeCheck,
                             "cannot infer type parameter '" + typeParam.name + "'"));
      }
      typeArgs.push_back(bound->second);
    }
    return typeArgs;
  }

  Type StructChecker::checkConcreteStructLiteral(ExpressionChecker& expressions,
                                                 const ast::StructLiteral& literal,
                                                 const StructInfo& structInfo,
                                                 std::vector<Type> typeArgs) {
    const std::unordered_map<std::string, Type> provided =
        checkStructLiteralFields(expressions, literal, structInfo);
    requireStructLiteralComplete(literal, structInfo, provided);
    return Type::structType(literal.structName, std::move(typeArgs));
  }

  std::unordered_map<std::string, Type>
  StructChecker::checkStructLiteralFields(ExpressionChecker& expressions,
                                          const ast::StructLiteral& literal,
                                          const StructInfo& structInfo) {
    std::unordered_map<std::string, Type> provided;
    for (const auto& field : literal.fields) {
      if (provided.contains(field.name)) {
        throw CompileError(formatDiagnostic(field.location, DiagnosticStage::TypeCheck,
                                            "duplicate field '" + field.name +
                                                "' in struct literal for '" + literal.structName +
                                                "'"));
      }

      if (!structInfo.fieldIndex.contains(field.name)) {
        throw CompileError(formatDiagnostic(field.location, DiagnosticStage::TypeCheck,
                                            "struct '" + literal.structName + "' has no field '" +
                                                field.name + "'"));
      }

      const StructFieldInfo& fieldInfo = structInfo.fields.at(structInfo.fieldIndex.at(field.name));
      requireFieldVisible(literal.structName, fieldInfo, field.location);
      provided.emplace(field.name, expressions.checkRvalue(*field.value, fieldInfo.type));
    }
    return provided;
  }

  void StructChecker::requireStructLiteralComplete(
      const ast::StructLiteral& literal, const StructInfo& structInfo,
      const std::unordered_map<std::string, Type>& provided) const {
    for (const auto& expectedField : structInfo.fields) {
      const auto actual = provided.find(expectedField.name);
      if (actual == provided.end()) {
        throw CompileError(formatDiagnostic(literal.location, DiagnosticStage::TypeCheck,
                                            "struct literal for '" + literal.structName +
                                                "' is missing field '" + expectedField.name + "'"));
      }

      if (!relations_.isAssignable(expectedField.type, actual->second)) {
        throw CompileError(formatDiagnostic(
            literal.location, DiagnosticStage::TypeCheck,
            "field '" + expectedField.name + "' of '" + literal.structName + "' expects " +
                expectedField.type.name() + ", got " + actual->second.name()));
      }
    }
  }

  StructInfo StructChecker::resolveStructInfo(const Type& structType,
                                                         SourceLocation location) const {
    if (structType.kind() != TypeKind::Struct) {
      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                          "internal: resolveStructInfo requires struct type"));
    }

    if (structType.typeArguments().empty()) {
      return lookupStruct(structType.structName(), location);
    }

    const auto genericStruct = environment().genericStructs.find(structType.structName());
    if (genericStruct == environment().genericStructs.end()) {
      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                          "unknown type '" + structType.name() + "'"));
    }

    const ast::StructDecl& templated = declarations_.genericStructAt(genericStruct->second);
    if (structType.typeArguments().size() != templated.typeParams.size()) {
      std::ostringstream out;
      out << "type '" << structType.name() << "' expects " << templated.typeParams.size()
          << " type argument(s), got " << structType.typeArguments().size();
      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck, out.str()));
    }

    Substitution substitution;
    for (std::size_t index{}; index < templated.typeParams.size(); ++index) {
      substitution.emplace(templated.typeParams[index].name, structType.typeArguments()[index]);
    }

    StructInfo info;
    info.fieldIndex.reserve(templated.fields.size());
    for (const auto& field : templated.fields) {
      const std::size_t fieldIndex = info.fields.size();
      const Type fieldType = substitute(field.type, substitution);
      info.fields.push_back(StructFieldInfo{field.name, fieldType, fieldIndex, field.visibility});
      info.fieldIndex.emplace(field.name, fieldIndex);
    }

    return info;
  }

  const StructInfo& StructChecker::lookupStruct(const std::string& name,
                                                           SourceLocation location) const {
    const auto structInfo = environment().structs.find(name);
    if (structInfo == environment().structs.end()) {
      throw CompileError(
          formatDiagnostic(location, DiagnosticStage::TypeCheck, "unknown type '" + name + "'"));
    }

    return structInfo->second;
  }

  void StructChecker::checkStructAcyclic(const std::string& structName,
                                       SourceLocation location) const {
    std::vector<const std::string*> stack;
    std::unordered_set<std::string> visiting;

    const auto visitStruct = [&](const auto& visitStructRef, const std::string& name) -> void {
      if (!environment().structs.contains(name)) {
        return;
      }

      if (!visiting.insert(name).second)
        return;

      stack.push_back(&name);
      if (stack.size() > environment().structs.size()) {
        throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                            "struct '" + structName + "' has infinite size"));
      }

      const StructInfo& info = environment().structs.at(name);
      for (const auto& field : info.fields) {
        if (field.type.kind() == TypeKind::Struct) {
          for (const std::string* seen : stack) {
            if (*seen == field.type.structName()) {
              throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                                  "struct '" + structName + "' has infinite size"));
            }
          }
          visitStructRef(visitStructRef, field.type.structName());
        }
      }

      stack.pop_back();
      visiting.erase(name);
    };

    visitStruct(visitStruct, structName);
  }

  void StructChecker::collectStructDecls(const ast::Module& module) {
    environment().structs.clear();
    environment().genericStructs.clear();
    environment().genericStructs.reserve(module.structs.size());

    for (std::size_t index{}; index < module.structs.size(); ++index) {
      const ast::StructDecl& decl = module.structs[index];
      if (environment().structs.contains(decl.name) ||
          environment().genericStructs.contains(decl.name)) {
        throw CompileError(formatDiagnostic(decl.location, DiagnosticStage::TypeCheck,
                                            "duplicate struct '" + decl.name + "'"));
      }

      if (!decl.typeParams.empty()) {
        collectGenericStructDecl(decl, index);
        continue;
      }

      collectConcreteStructDecl(decl);
    }

    validateConcreteStructFieldTypes(module);
  }

  void StructChecker::collectGenericStructDecl(const ast::StructDecl& decl, std::size_t moduleIndex) {
    std::unordered_set<std::string> allowedTypeParams;
    for (const auto& typeParam : decl.typeParams) {
      allowedTypeParams.insert(typeParam.name);
    }

    std::unordered_set<std::string> seenFields;
    for (const auto& field : decl.fields) {
      if (seenFields.contains(field.name)) {
        throw CompileError(
            formatDiagnostic(field.location, DiagnosticStage::TypeCheck,
                             "duplicate field '" + field.name + "' in struct '" + decl.name + "'"));
      }
      seenFields.insert(field.name);
      relations_.requireKnownType(field.type, field.location, &allowedTypeParams, false,
                                  allowsInternalStructTypes(decl));
    }

    environment().genericStructs.emplace(decl.name, moduleIndex);
  }

  void StructChecker::collectConcreteStructDecl(const ast::StructDecl& decl) {
    if (environment().structs.contains(decl.name) ||
        environment().genericStructs.contains(decl.name)) {
      throw CompileError(formatDiagnostic(decl.location, DiagnosticStage::TypeCheck,
                                          "duplicate struct '" + decl.name + "'"));
    }

    StructInfo info;
    info.fieldIndex.reserve(decl.fields.size());
    std::unordered_set<std::string> seenFields;
    for (const auto& field : decl.fields) {
      if (seenFields.contains(field.name)) {
        throw CompileError(
            formatDiagnostic(field.location, DiagnosticStage::TypeCheck,
                             "duplicate field '" + field.name + "' in struct '" + decl.name + "'"));
      }
      seenFields.insert(field.name);

      const std::size_t index = info.fields.size();
      info.fields.push_back(StructFieldInfo{field.name, field.type, index, field.visibility});
      info.fieldIndex.emplace(field.name, index);
    }

    environment().structs.emplace(decl.name, std::move(info));
  }

  void StructChecker::validateConcreteStructFieldTypes(const ast::Module& module,
                                                     std::size_t firstStruct) {
    for (std::size_t index = firstStruct; index < module.structs.size(); ++index) {
      const ast::StructDecl& decl = module.structs[index];
      if (!decl.typeParams.empty()) {
        continue;
      }

      for (const auto& field : decl.fields) {
        relations_.requireKnownType(field.type, field.location, nullptr, false,
                                    allowsInternalStructTypes(decl));
      }
      checkStructAcyclic(decl.name, decl.location);
    }
  }

  bool StructChecker::allowsInternalStructTypes(const ast::StructDecl& decl) const {
    const auto origin = environment().symbolOrigins.structs.find(decl.name);
    return origin != environment().symbolOrigins.structs.end() &&
           declarations_.isStdlibOrigin(origin->second);
  }

  std::string StructChecker::structOriginModule(const std::string& structName) const {
    const auto origin = environment().symbolOrigins.structs.find(structName);
    if (origin == environment().symbolOrigins.structs.end()) {
      return "";
    }
    return origin->second;
  }

  std::string StructChecker::currentModuleOrigin() const {
    const auto origin = environment().symbolOrigins.functions.find(session().currentFunctionName);
    if (origin == environment().symbolOrigins.functions.end()) {
      return "";
    }
    return origin->second;
  }

  void StructChecker::requireFieldVisible(const std::string& structName, const StructFieldInfo& field,
                                        SourceLocation location) const {
    if (field.visibility == ast::FieldVisibility::Public) {
      return;
    }

    const std::string declaringModule = structOriginModule(structName);
    const std::string useModule = currentModuleOrigin();
    if (declaringModule == useModule) {
      return;
    }

    throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                        "field '" + field.name + "' is private to module '" +
                                            declaringModule + "'"));
  }

  void StructChecker::requireDefaultInitializable(const Type& type, SourceLocation location) {
    const Type canonical = relations_.canonicalStructType(type);
    if (const std::optional<StandardContainer> container = relations_.standardContainerFor(canonical)) {
      relations_.recordImplicitContainerOperation(*container, ContainerOperation::New,
                                                  canonical.typeArguments(), location);
      relations_.requireContainerOwnershipOps(canonical, location);
      return;
    }

    if (canonical.kind() != TypeKind::Struct) {
      return;
    }

    const StructInfo info = resolveStructInfo(canonical, location);
    for (const StructFieldInfo& field : info.fields) {
      requireDefaultInitializable(field.type, location);
    }
  }

} // namespace noria
