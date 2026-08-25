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

  void TypeChecker::TypeRelations::requireKnownType(
      const Type& type, SourceLocation location,
      const std::unordered_set<std::string>* allowedTypeParams, bool allowImplTags,
      bool allowInternalTypes) const {
    if (type == Type::i32() || type == Type::f64() || type == Type::boolean() ||
        type == Type::str())
      return;

    if (type == Type::voidType()) {
      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                          "void is only valid as a function return type"));
    }

    if (type.kind == TypeKind::RawPtr) {
      requireRawPtrUsable(type, location, allowInternalTypes);
      return;
    }

    if (type.kind == TypeKind::ImplTag) {
      requireImplTagUsable(type, location, allowImplTags);
      return;
    }

    if (type.kind == TypeKind::TypeParam) {
      requireTypeParamKnown(type, location, allowedTypeParams);
      return;
    }

    if (type.kind == TypeKind::Array) {
      requireArrayTypeKnown(type, location, allowedTypeParams, allowImplTags, allowInternalTypes);
      return;
    }

    if (type.kind == TypeKind::Struct) {
      requireStructTypeKnown(type, location, allowedTypeParams, allowInternalTypes);
      return;
    }

    throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                        "unknown type '" + type.name() + "'"));
  }

  void TypeChecker::TypeRelations::requireRawPtrUsable(const Type&, SourceLocation location,
                                                             bool allowInternalTypes) const {
    if (!allowInternalTypes) {
      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                          "__rt_ptr cannot be used outside the standard library"));
    }
  }

  void TypeChecker::TypeRelations::requireImplTagUsable(const Type& type,
                                                              SourceLocation location,
                                                              bool allowImplTags) const {
    if (allowImplTags) {
      return;
    }
    throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                        "implementation tag '" +
                                            std::string(implementationTagName(type.implTag)) +
                                            "' cannot be used as a type"));
  }

  void TypeChecker::TypeRelations::requireTypeParamKnown(
      const Type& type, SourceLocation location,
      const std::unordered_set<std::string>* allowedTypeParams) const {
    if (allowedTypeParams != nullptr && allowedTypeParams->contains(type.typeParamName)) {
      return;
    }
    throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                        "unresolved type parameter '" + type.typeParamName + "'"));
  }

  void TypeChecker::TypeRelations::requireArrayTypeKnown(
      const Type& type, SourceLocation location,
      const std::unordered_set<std::string>* allowedTypeParams, bool allowImplTags,
      bool allowInternalTypes) const {
    if (!type.element) {
      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                          "unknown type '" + type.name() + "'"));
    }
    requireKnownType(*type.element, location, allowedTypeParams, allowImplTags, allowInternalTypes);
    rejectStructArrayElement(*type.element, location);
  }

  void TypeChecker::TypeRelations::requireStructTypeKnown(
      const Type& type, SourceLocation location,
      const std::unordered_set<std::string>* allowedTypeParams, bool allowInternalTypes) const {
    if (!type.typeArgs.empty()) {
      const auto genericStruct = checker_.environment_.genericStructs.find(type.structName);
      if (genericStruct == checker_.environment_.genericStructs.end()) {
        if (checker_.environment_.structs.contains(type.structName)) {
          throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                              "type '" + type.name() +
                                                  "' is not generic and cannot take type "
                                                  "arguments"));
        }
        throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                            "unknown type '" + type.name() + "'"));
      }

      const ast::StructDecl& templated = checker_.genericStructAt(genericStruct->second);
      if (type.typeArgs.size() != templated.typeParams.size()) {
        std::ostringstream out;
        out << "type '" << type.name() << "' expects " << templated.typeParams.size()
            << " type argument(s), got " << type.typeArgs.size();
        throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck, out.str()));
      }

      for (const Type& typeArg : type.typeArgs) {
        requireKnownType(typeArg, location, allowedTypeParams, true, allowInternalTypes);
      }

      if (!containsUnboundTypeParam(type)) {
        recordStructSpecialization(type.structName, type.typeArgs, location);
      }
      return;
    }

    if (!checker_.environment_.structs.contains(type.structName)) {
      if (checker_.environment_.genericStructs.contains(type.structName)) {
        throw CompileError(
            formatDiagnostic(location, DiagnosticStage::TypeCheck,
                             "generic struct '" + type.structName + "' requires type arguments"));
      }
      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                          "unknown type '" + type.name() + "'"));
    }
  }

  void TypeChecker::TypeRelations::unifyTypes(const Type& expected, const Type& actual,
                                                    std::unordered_map<std::string, Type>& bindings,
                                                    SourceLocation location) const {
    if (expected.kind == TypeKind::TypeParam) {
      bindTypeParam(expected, actual, bindings, location);
      return;
    }

    if (expected.kind == TypeKind::Array) {
      unifyArrayTypes(expected, actual, bindings, location);
      return;
    }

    if (expected.kind == TypeKind::ImplTag) {
      unifyImplTagTypes(expected, actual, location);
      return;
    }

    if (expected.kind == TypeKind::Struct) {
      unifyStructTypes(expected, actual, bindings, location);
      return;
    }

    if (!isAssignable(expected, actual)) {
      throw CompileError(
          formatDiagnostic(location, DiagnosticStage::TypeCheck,
                           "expected " + expected.name() + ", got " + actual.name()));
    }
  }

  void
  TypeChecker::TypeRelations::bindTypeParam(const Type& expected, const Type& actual,
                                                  std::unordered_map<std::string, Type>& bindings,
                                                  SourceLocation location) const {
    const auto existing = bindings.find(expected.typeParamName);
    if (existing == bindings.end()) {
      bindings.emplace(expected.typeParamName, actual);
      return;
    }

    if (existing->second != actual) {
      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                          "conflicting types " + existing->second.name() + " and " +
                                              actual.name() + " for type parameter '" +
                                              expected.typeParamName + "'"));
    }
  }

  void
  TypeChecker::TypeRelations::unifyArrayTypes(const Type& expected, const Type& actual,
                                                    std::unordered_map<std::string, Type>& bindings,
                                                    SourceLocation location) const {
    if (actual.kind != TypeKind::Array || !expected.element || !actual.element) {
      throw CompileError(
          formatDiagnostic(location, DiagnosticStage::TypeCheck,
                           "expected " + expected.name() + ", got " + actual.name()));
    }
    unifyTypes(*expected.element, *actual.element, bindings, location);
  }

  void TypeChecker::TypeRelations::unifyImplTagTypes(const Type& expected, const Type& actual,
                                                           SourceLocation location) const {
    if (actual.kind != TypeKind::ImplTag || expected.implTag != actual.implTag) {
      throw CompileError(
          formatDiagnostic(location, DiagnosticStage::TypeCheck,
                           "expected " + expected.name() + ", got " + actual.name()));
    }
  }

  Type TypeChecker::TypeRelations::canonicalStructType(const Type& type) const {
    if (type.kind != TypeKind::Struct || !type.typeArgs.empty()) {
      return type;
    }

    const auto specialization =
        checker_.session_.structSpecializationTypeArgs.find(type.structName);
    if (specialization == checker_.session_.structSpecializationTypeArgs.end()) {
      return type;
    }

    const std::size_t dollar = type.structName.find('$');
    if (dollar == std::string::npos) {
      return type;
    }

    return Type::structType(type.structName.substr(0, dollar), specialization->second);
  }

  void TypeChecker::TypeRelations::unifyStructTypes(
      const Type& expected, const Type& actual, std::unordered_map<std::string, Type>& bindings,
      SourceLocation location) const {
    if (actual.kind != TypeKind::Struct) {
      throw CompileError(
          formatDiagnostic(location, DiagnosticStage::TypeCheck,
                           "expected " + expected.name() + ", got " + actual.name()));
    }

    const Type expectedStruct = canonicalStructType(expected);
    const Type actualStruct = canonicalStructType(actual);
    if (expectedStruct.structName == actualStruct.structName &&
        expectedStruct.typeArgs.size() == actualStruct.typeArgs.size()) {
      for (std::size_t index{}; index < expectedStruct.typeArgs.size(); ++index) {
        unifyTypes(expectedStruct.typeArgs[index], actualStruct.typeArgs[index], bindings,
                   location);
      }
      return;
    }

    if (structSpecializationsMatch(expected, actual)) {
      return;
    }

    throw CompileError(
        formatDiagnostic(location, DiagnosticStage::TypeCheck,
                         "expected " + expected.name() + ", got " + actual.name()));
  }

  void TypeChecker::TypeRelations::checkSpecializationConstraints(
      const std::string& templateName, const std::vector<Type>& typeArgs,
      SourceLocation location) const {
    (void)templateName;

    std::optional<ImplementationTag> tag;
    for (const Type& typeArg : typeArgs) {
      if (typeArg.kind == TypeKind::ImplTag) {
        tag = typeArg.implTag;
        break;
      }
    }
    if (!tag) {
      return;
    }

    std::optional<Type> keyType;
    for (const Type& typeArg : typeArgs) {
      if (typeArg.kind != TypeKind::ImplTag) {
        keyType = typeArg;
        break;
      }
    }
    if (!keyType) {
      return;
    }

    for (const RequiredOperation operation : requiredOperations(*tag)) {
      if (supportsOperation(*keyType, operation)) {
        continue;
      }

      std::ostringstream out;
      out << "implementation tag '" << implementationTagName(*tag) << "' requires '"
          << operationName(operation) << "' for key type " << keyType->name();
      if (operation == RequiredOperation::Hash) {
        out << "; V2 hashes i32, bool, str";
      }
      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck, out.str()));
    }
  }

  void
  TypeChecker::TypeRelations::recordStructSpecialization(const std::string& templateName,
                                                               const std::vector<Type>& typeArgs,
                                                               SourceLocation location) const {
    checkSpecializationConstraints(templateName, typeArgs, location);
    checker_.registerStructSpecialization(mangleSpecialization(templateName, typeArgs), typeArgs);
    checker_.session_.structSpecializationRequests.push_back(StructSpecializationRequest{
        templateName, typeArgs, location, checker_.session_.currentFunctionName});
  }
  bool TypeChecker::TypeRelations::isAssignable(Type expected, Type actual) const {
    if (expected == actual) {
      return true;
    }
    return structSpecializationsMatch(expected, actual);
  }

} // namespace noria
