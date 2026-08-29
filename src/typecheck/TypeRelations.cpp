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

  void TypeRelations::requireKnownType(
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

    if (type.kind() == TypeKind::RawPtr) {
      requireRawPtrUsable(type, location, allowInternalTypes);
      return;
    }

    if (type.kind() == TypeKind::ImplTag) {
      requireImplTagUsable(type, location, allowImplTags);
      return;
    }

    if (type.kind() == TypeKind::TypeParam) {
      requireTypeParamKnown(type, location, allowedTypeParams);
      return;
    }

    if (type.kind() == TypeKind::Array) {
      requireArrayTypeKnown(type, location, allowedTypeParams, allowImplTags, allowInternalTypes);
      return;
    }

    if (type.kind() == TypeKind::Struct) {
      requireStructTypeKnown(type, location, allowedTypeParams, allowInternalTypes);
      return;
    }

    throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                        "unknown type '" + type.name() + "'"));
  }

  void TypeRelations::requireRawPtrUsable(const Type&, SourceLocation location,
                                                             bool allowInternalTypes) const {
    if (!allowInternalTypes) {
      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                          "__rt_ptr cannot be used outside the standard library"));
    }
  }

  void TypeRelations::requireImplTagUsable(const Type& type,
                                                              SourceLocation location,
                                                              bool allowImplTags) const {
    if (allowImplTags) {
      return;
    }
    throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                        "implementation tag '" +
                                            std::string(implementationTagName(type.implementationTagValue())) +
                                            "' cannot be used as a type"));
  }

  void TypeRelations::requireTypeParamKnown(
      const Type& type, SourceLocation location,
      const std::unordered_set<std::string>* allowedTypeParams) const {
    if (allowedTypeParams != nullptr && allowedTypeParams->contains(type.typeParameterName())) {
      return;
    }
    throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                        "unresolved type parameter '" + type.typeParameterName() + "'"));
  }

  void TypeRelations::requireArrayTypeKnown(
      const Type& type, SourceLocation location,
      const std::unordered_set<std::string>* allowedTypeParams, bool allowImplTags,
      bool allowInternalTypes) const {
    requireKnownType(type.elementType(), location, allowedTypeParams, allowImplTags,
                     allowInternalTypes);
    rejectStructArrayElement(type.elementType(), location);
  }

  void TypeRelations::requireStructTypeKnown(
      const Type& type, SourceLocation location,
      const std::unordered_set<std::string>* allowedTypeParams, bool allowInternalTypes) const {
    if (!type.typeArguments().empty()) {
      const auto genericStruct = environment().genericStructs.find(type.structName());
      if (genericStruct == environment().genericStructs.end()) {
        if (environment().structs.contains(type.structName())) {
          throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                              "type '" + type.name() +
                                                  "' is not generic and cannot take type "
                                                  "arguments"));
        }
        throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                            "unknown type '" + type.name() + "'"));
      }

      if (environment().activeModule == nullptr ||
          genericStruct->second >= environment().activeModule->structs.size()) {
        throw CompileError("typecheck: internal error: invalid generic struct index");
      }
      const ast::StructDecl& templated = environment().activeModule->structs[genericStruct->second];
      if (type.typeArguments().size() != templated.typeParams.size()) {
        std::ostringstream out;
        out << "type '" << type.name() << "' expects " << templated.typeParams.size()
            << " type argument(s), got " << type.typeArguments().size();
        throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck, out.str()));
      }

      for (const Type& typeArg : type.typeArguments()) {
        requireKnownType(typeArg, location, allowedTypeParams, true, allowInternalTypes);
      }

      if (!containsUnboundTypeParam(type)) {
        recordStructSpecialization(type.structName(), type.typeArguments(), location);
      }
      return;
    }

    if (!environment().structs.contains(type.structName())) {
      if (environment().genericStructs.contains(type.structName())) {
        throw CompileError(
            formatDiagnostic(location, DiagnosticStage::TypeCheck,
                             "generic struct '" + type.structName() + "' requires type arguments"));
      }
      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                          "unknown type '" + type.name() + "'"));
    }
  }

  void TypeRelations::unifyTypes(const Type& expected, const Type& actual,
                                                    std::unordered_map<std::string, Type>& bindings,
                                                    SourceLocation location) const {
    if (expected.kind() == TypeKind::TypeParam) {
      bindTypeParam(expected, actual, bindings, location);
      return;
    }

    if (expected.kind() == TypeKind::Array) {
      unifyArrayTypes(expected, actual, bindings, location);
      return;
    }

    if (expected.kind() == TypeKind::ImplTag) {
      unifyImplTagTypes(expected, actual, location);
      return;
    }

    if (expected.kind() == TypeKind::Struct) {
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
  TypeRelations::bindTypeParam(const Type& expected, const Type& actual,
                                                  std::unordered_map<std::string, Type>& bindings,
                                                  SourceLocation location) const {
    const auto existing = bindings.find(expected.typeParameterName());
    if (existing == bindings.end()) {
      bindings.emplace(expected.typeParameterName(), actual);
      return;
    }

    if (existing->second != actual) {
      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                          "conflicting types " + existing->second.name() + " and " +
                                              actual.name() + " for type parameter '" +
                                              expected.typeParameterName() + "'"));
    }
  }

  void
  TypeRelations::unifyArrayTypes(const Type& expected, const Type& actual,
                                                    std::unordered_map<std::string, Type>& bindings,
                                                    SourceLocation location) const {
    if (actual.kind() != TypeKind::Array) {
      throw CompileError(
          formatDiagnostic(location, DiagnosticStage::TypeCheck,
                           "expected " + expected.name() + ", got " + actual.name()));
    }
    unifyTypes(expected.elementType(), actual.elementType(), bindings, location);
  }

  void TypeRelations::unifyImplTagTypes(const Type& expected, const Type& actual,
                                                           SourceLocation location) const {
    if (actual.kind() != TypeKind::ImplTag ||
        expected.implementationTagValue() != actual.implementationTagValue()) {
      throw CompileError(
          formatDiagnostic(location, DiagnosticStage::TypeCheck,
                           "expected " + expected.name() + ", got " + actual.name()));
    }
  }

  Type TypeRelations::canonicalStructType(const Type& type) const {
    if (type.kind() != TypeKind::Struct || !type.typeArguments().empty()) {
      return type;
    }

    const auto specialization = specializations().structTypeArgs().find(type.structName());
    if (specialization == specializations().structTypeArgs().end()) {
      return type;
    }

    const std::size_t dollar = type.structName().find('$');
    if (dollar == std::string::npos) {
      return type;
    }

    return Type::structType(type.structName().substr(0, dollar), specialization->second);
  }

  void TypeRelations::unifyStructTypes(
      const Type& expected, const Type& actual, std::unordered_map<std::string, Type>& bindings,
      SourceLocation location) const {
    if (actual.kind() != TypeKind::Struct) {
      throw CompileError(
          formatDiagnostic(location, DiagnosticStage::TypeCheck,
                           "expected " + expected.name() + ", got " + actual.name()));
    }

    const Type expectedStruct = canonicalStructType(expected);
    const Type actualStruct = canonicalStructType(actual);
    if (expectedStruct.structName() == actualStruct.structName() &&
        expectedStruct.typeArguments().size() == actualStruct.typeArguments().size()) {
      for (std::size_t index{}; index < expectedStruct.typeArguments().size(); ++index) {
        unifyTypes(expectedStruct.typeArguments()[index], actualStruct.typeArguments()[index], bindings,
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

  void TypeRelations::checkSpecializationConstraints(
      const std::string& templateName, const std::vector<Type>& typeArgs,
      SourceLocation location) const {
    (void)templateName;

    std::optional<ImplementationTag> tag;
    for (const Type& typeArg : typeArgs) {
      if (typeArg.kind() == TypeKind::ImplTag) {
        tag = typeArg.implementationTagValue();
        break;
      }
    }
    if (!tag) {
      return;
    }

    std::optional<Type> keyType;
    for (const Type& typeArg : typeArgs) {
      if (typeArg.kind() != TypeKind::ImplTag) {
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
  TypeRelations::recordStructSpecialization(const std::string& templateName,
                                                               const std::vector<Type>& typeArgs,
                                                               SourceLocation location) const {
    checkSpecializationConstraints(templateName, typeArgs, location);
    specializations().registerStruct(mangleSpecialization(templateName, typeArgs), typeArgs);
    specializations().recordStructRequest(
        StructSpecializationRequest{templateName, typeArgs, location, session().currentFunctionName});
  }
  bool TypeRelations::isAssignable(Type expected, Type actual) const {
    if (expected == actual) {
      return true;
    }
    return structSpecializationsMatch(expected, actual);
  }

  std::optional<StandardContainer> TypeRelations::standardContainerFor(const Type& type) const {
    const Type canonical = canonicalStructType(type);
    if (canonical.kind() != TypeKind::Struct) {
      return std::nullopt;
    }

    const auto origin = environment().symbolOrigins.structs.find(canonical.structName());
    const std::string module = origin == environment().symbolOrigins.structs.end() ? "" : origin->second;
    const StandardContainerInfo* info = standardContainerInfo(module, canonical.structName());
    if (info == nullptr || canonical.typeArguments().size() != info->typeArgumentCount) {
      return std::nullopt;
    }
    return info->kind;
  }

  void TypeRelations::recordImplicitContainerOperation(StandardContainer container,
                                                       ContainerOperation operation,
                                                       const std::vector<Type>& typeArgs,
                                                       SourceLocation location) {
    const std::string name(containerOperationHiddenName(container, operation));
    if (name.empty() || !environment().genericFunctions.contains(name)) {
      throw CompileError("typecheck: internal error: missing container operation");
    }
    checkSpecializationConstraints(name, typeArgs, location);
    specializations().recordFunctionRequest(
        SpecializationRequest{name, typeArgs, location, session().currentFunctionName, false});
  }

  void TypeRelations::requireContainerOwnershipOps(const Type& type, SourceLocation location) {
    const Type canonical = canonicalStructType(type);
    if (const std::optional<StandardContainer> container = standardContainerFor(canonical)) {
      recordImplicitContainerOperation(*container, ContainerOperation::Drop, canonical.typeArguments(),
                                       location);
      recordImplicitContainerOperation(*container, ContainerOperation::Clone, canonical.typeArguments(),
                                       location);
    }
  }

} // namespace noria
