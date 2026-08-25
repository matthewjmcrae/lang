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

  void TypeChecker::ExpressionVisitor::visit(const ast::CallExpression& call) {
    const auto strategy = checker_.activate(TypeCheckerStrategyKind::Calls);
    if (const BuiltinSignature* descriptor = lookupBuiltin(call.callee)) {
      result_ = checker_.checkBuiltinCall(call, *descriptor);
      return;
    }

    checker_.requireFunctionCallable(call.callee, call.location);

    const auto concreteFunction = checker_.environment_.functions.find(call.callee);
    if (concreteFunction != checker_.environment_.functions.end()) {
      result_ = checker_.checkConcreteFunctionCall(call, concreteFunction->second);
      return;
    }

    const auto genericFunction = checker_.environment_.genericFunctions.find(call.callee);
    if (genericFunction == checker_.environment_.genericFunctions.end()) {
      throw CompileError(formatDiagnostic(call.location, DiagnosticStage::TypeCheck,
                                          "unknown function '" + call.callee + "'"));
    }

    result_ = checker_.checkGenericFunctionCall(call, genericFunction->second, expectedType_);
  }

  Type TypeChecker::checkConcreteFunctionCall(const ast::CallExpression& call,
                                                    const FunctionSignature& signature) {
    if (call.arguments.size() != signature.parameterTypes.size()) {
      std::ostringstream out;
      out << "function '" << call.callee << "' expects " << signature.parameterTypes.size()
          << " argument(s), got " << call.arguments.size();
      throw CompileError(formatDiagnostic(call.location, DiagnosticStage::TypeCheck, out.str()));
    }

    for (std::size_t index{}; index < call.arguments.size(); ++index) {
      const Type actual = checkRvalue(*call.arguments[index]);
      const Type expected = signature.parameterTypes[index];
      if (!isAssignable(expected, actual)) {
        std::ostringstream out;
        out << "argument " << (index + 1) << " of '" << call.callee << "' expects "
            << expected.name() << ", got " << actual.name();
        throw CompileError(formatDiagnostic(call.arguments[index]->location,
                                            DiagnosticStage::TypeCheck, out.str()));
      }
    }

    return signature.returnType;
  }

  Type TypeChecker::checkGenericFunctionCall(const ast::CallExpression& call,
                                                   const std::vector<std::size_t>& family,
                                                   const std::optional<Type>& expectedType) {
    const bool calleeHasImplTags =
        std::any_of(family.begin(), family.end(), [&](std::size_t candidateIndex) {
          return genericFunctionAt(candidateIndex).implTag.has_value();
        });
    const bool specializedNestedImplCall =
        calleeHasImplTags && enclosingFunctionSpecializationTypeArgs() != nullptr;

    const ast::Function& signature = genericFunctionAt(family.front());
    std::unordered_map<std::string, Type> bindings;
    const std::vector<Type> typeArgs = inferGenericCallTypeArgs(
        call, signature, specializedNestedImplCall, expectedType, bindings);

    if (environment_.activeModule == nullptr) {
      throw CompileError(
          "typecheck: internal error: generic function lookup without active module");
    }
    const ast::Function* selected = selectGenericImplementation(
        *environment_.activeModule, family, findImplTag(typeArgs), call.callee, call.location);

    Substitution substitution;
    for (const auto& typeParam : selected->typeParams) {
      substitution.emplace(typeParam.name, bindings.at(typeParam.name));
    }

    checkSpecializationConstraints(call.callee, typeArgs, call.location);
    session_.specializationRequests.push_back(
        SpecializationRequest{call.callee, typeArgs, call.location, session_.currentFunctionName});
    return substitute(selected->returnType, substitution);
  }

  std::vector<Type> TypeChecker::inferGenericCallTypeArgs(
      const ast::CallExpression& call, const ast::Function& signature,
      bool seedFromSpecializedCaller, const std::optional<Type>& expectedType,
      std::unordered_map<std::string, Type>& bindings) {
    if (call.arguments.size() != signature.parameters.size()) {
      std::ostringstream out;
      out << "function '" << call.callee << "' expects " << signature.parameters.size()
          << " argument(s), got " << call.arguments.size();
      throw CompileError(formatDiagnostic(call.location, DiagnosticStage::TypeCheck, out.str()));
    }

    if (seedFromSpecializedCaller) {
      seedMatchingTypeParamsFromCaller(bindings, signature.typeParams);
    }
    for (std::size_t index{}; index < call.arguments.size(); ++index) {
      const Type actual = checkRvalue(*call.arguments[index]);
      Type expectedParam = signature.parameters[index].type;
      if (seedFromSpecializedCaller) {
        Substitution substitution(bindings.begin(), bindings.end());
        if (allTypeParamsSubstituted(expectedParam, substitution)) {
          expectedParam = substituteSpecializationType(expectedParam, substitution);
        }
      }
      unifyTypes(expectedParam, actual, bindings, call.arguments[index]->location);
    }

    seedMatchingTypeParamsFromCaller(bindings, signature.typeParams);
    seedUnboundTypeParamsFromExpectedType(bindings, signature.returnType, expectedType,
                                          call.location);
    seedUnboundTypeParamsFromCaller(bindings, signature.typeParams);

    std::vector<Type> typeArgs;
    typeArgs.reserve(signature.typeParams.size());
    for (const auto& typeParam : signature.typeParams) {
      const auto bound = bindings.find(typeParam.name);
      if (bound == bindings.end()) {
        throw CompileError(
            formatDiagnostic(call.location, DiagnosticStage::TypeCheck,
                             "cannot infer type parameter '" + typeParam.name + "'"));
      }
      typeArgs.push_back(bound->second);
    }

    return typeArgs;
  }

  Type TypeChecker::checkBuiltinCall(const ast::CallExpression& call,
                                           const BuiltinSignature& descriptor) {
    requireBuiltinCallable(call, descriptor);

    if (descriptor.id == BuiltinId::Len) {
      return checkLenBuiltin(call);
    }

    if (descriptor.id == BuiltinId::RtSizeof) {
      return checkRtSizeofBuiltin(call);
    }

    if (descriptor.id == BuiltinId::RtHash) {
      return checkRtHashBuiltin(call);
    }

    if (descriptor.id == BuiltinId::RtLoad) {
      return checkRtLoadBuiltin(call, descriptor);
    }

    if (descriptor.id == BuiltinId::RtStore) {
      return checkRtStoreBuiltin(call, descriptor);
    }

    if (descriptor.style == MismatchStyle::AllArguments) {
      return checkAllArgumentsBuiltin(call, descriptor);
    }

    return checkDeclaredBuiltinArguments(call, descriptor);
  }

  void TypeChecker::requireBuiltinCallable(const ast::CallExpression& call,
                                                 const BuiltinSignature& descriptor) const {
    if (descriptor.visibility == Visibility::Internal && !isStdlibContext()) {
      throw CompileError(formatDiagnostic(call.location, DiagnosticStage::TypeCheck,
                                          "internal runtime builtin '" +
                                              std::string(descriptor.name) +
                                              "' is unavailable outside the standard library"));
    }

    if (!builtinArityMatches(descriptor, call.arguments.size())) {
      throw CompileError(formatDiagnostic(call.location, DiagnosticStage::TypeCheck,
                                          formatBuiltinArityError(descriptor)));
    }
  }

  Type TypeChecker::checkLenBuiltin(const ast::CallExpression& call) {
    const Type actual = checkRvalue(*call.arguments[0]);
    if (actual == Type::str() || actual.kind == TypeKind::Array)
      return Type::i32();

    throw CompileError(formatDiagnostic(call.arguments[0]->location, DiagnosticStage::TypeCheck,
                                        "len expects str or array, got " + actual.name()));
  }

  Type TypeChecker::checkRtSizeofBuiltin(const ast::CallExpression& call) const {
    const Type witness = resolveWitnessType(call.location);
    if (!isScalarWitnessType(witness)) {
      throw CompileError(
          formatDiagnostic(call.location, DiagnosticStage::TypeCheck,
                           "__rt_sizeof requires a scalar element type, got " + witness.name()));
    }
    return Type::i32();
  }

  Type TypeChecker::checkRtHashBuiltin(const ast::CallExpression& call) {
    const Type witness = resolveWitnessType(call.location);
    if (!supportsOperation(witness, RequiredOperation::Hash)) {
      throw CompileError(formatDiagnostic(
          call.location, DiagnosticStage::TypeCheck,
          "__rt_hash requires a hashable key type (i32, bool, str), got " + witness.name()));
    }
    const Type value = checkRvalue(*call.arguments[0]);
    if (value != witness) {
      throw CompileError(
          formatDiagnostic(call.arguments[0]->location, DiagnosticStage::TypeCheck,
                           "__rt_hash expects " + witness.name() + ", got " + value.name()));
    }
    return Type::i32();
  }

  Type TypeChecker::checkRtLoadBuiltin(const ast::CallExpression& call,
                                             const BuiltinSignature& descriptor) {
    const Type pointer = checkRvalue(*call.arguments[0]);
    const Type index = checkRvalue(*call.arguments[1]);
    if (pointer != Type::rawPtr()) {
      throw CompileError(formatDiagnostic(
          call.arguments[0]->location, DiagnosticStage::TypeCheck,
          formatBuiltinPerArgumentMismatch(descriptor.name, TypeKind::RawPtr, pointer.name())));
    }
    if (index != Type::i32()) {
      throw CompileError(formatDiagnostic(
          call.arguments[1]->location, DiagnosticStage::TypeCheck,
          formatBuiltinPerArgumentMismatch(descriptor.name, TypeKind::I32, index.name())));
    }

    // Runtime load/store builtins use the enclosing specialization as their element witness.
    const Type witness = resolveWitnessType(call.location);
    if (!isScalarWitnessType(witness)) {
      throw CompileError(
          formatDiagnostic(call.location, DiagnosticStage::TypeCheck,
                           "__rt_load requires a scalar element type, got " + witness.name()));
    }
    return witness;
  }

  Type TypeChecker::checkRtStoreBuiltin(const ast::CallExpression& call,
                                              const BuiltinSignature& descriptor) {
    const Type pointer = checkRvalue(*call.arguments[0]);
    const Type index = checkRvalue(*call.arguments[1]);
    const Type value = checkRvalue(*call.arguments[2]);
    if (pointer != Type::rawPtr()) {
      throw CompileError(formatDiagnostic(
          call.arguments[0]->location, DiagnosticStage::TypeCheck,
          formatBuiltinPerArgumentMismatch(descriptor.name, TypeKind::RawPtr, pointer.name())));
    }
    if (index != Type::i32()) {
      throw CompileError(formatDiagnostic(
          call.arguments[1]->location, DiagnosticStage::TypeCheck,
          formatBuiltinPerArgumentMismatch(descriptor.name, TypeKind::I32, index.name())));
    }

    const Type witness = resolveWitnessType(call.location);
    if (!isScalarWitnessType(witness)) {
      throw CompileError(
          formatDiagnostic(call.location, DiagnosticStage::TypeCheck,
                           "__rt_store requires a scalar element type, got " + witness.name()));
    }
    if (value != witness) {
      throw CompileError(
          formatDiagnostic(call.arguments[2]->location, DiagnosticStage::TypeCheck,
                           "__rt_store expects " + witness.name() + ", got " + value.name()));
    }
    return Type::voidType();
  }

  Type TypeChecker::checkAllArgumentsBuiltin(const ast::CallExpression& call,
                                                   const BuiltinSignature& descriptor) {
    const Type firstType = checkRvalue(*call.arguments[0]);
    const Type secondType = checkRvalue(*call.arguments[1]);
    const Type expected = Type(descriptor.parameters[0]);
    if (firstType != expected || secondType != expected) {
      throw CompileError(formatDiagnostic(
          call.location, DiagnosticStage::TypeCheck,
          formatBuiltinAllArgumentsMismatch(descriptor.name, descriptor.parameters[0],
                                            firstType.name(), secondType.name())));
    }
    return Type(descriptor.returnKind);
  }

  Type TypeChecker::checkDeclaredBuiltinArguments(const ast::CallExpression& call,
                                                        const BuiltinSignature& descriptor) {
    for (std::size_t index{}; index < descriptor.arity; ++index) {
      const Type actual = checkRvalue(*call.arguments[index]);
      const TypeKind expectedKind = descriptor.parameters[index];
      if (expectedKind == TypeKind::TypeParam) {
        continue;
      }
      const Type expected = Type(expectedKind);
      if (actual != expected) {
        throw CompileError(
            formatDiagnostic(call.arguments[index]->location, DiagnosticStage::TypeCheck,
                             formatBuiltinPerArgumentMismatch(
                                 descriptor.name, descriptor.parameters[index], actual.name())));
      }
    }

    if (descriptor.returnKind == TypeKind::TypeParam) {
      return resolveWitnessType(call.location);
    }

    return Type(descriptor.returnKind);
  }

  bool TypeChecker::isEnclosingFunctionSpecialized() const {
    return session_.functionSpecializationTypeArgs.contains(session_.currentFunctionName);
  }

  const std::vector<Type>* TypeChecker::enclosingFunctionSpecializationTypeArgs() const {
    const auto specialization =
        session_.functionSpecializationTypeArgs.find(session_.currentFunctionName);
    if (specialization == session_.functionSpecializationTypeArgs.end()) {
      return nullptr;
    }
    return &specialization->second;
  }

  void TypeChecker::seedMatchingTypeParamsFromCaller(
      std::unordered_map<std::string, Type>& bindings,
      const std::vector<ast::TypeParameter>& calleeTypeParams) const {
    const std::vector<Type>* callerTypeArgs = enclosingFunctionSpecializationTypeArgs();
    if (callerTypeArgs == nullptr) {
      return;
    }

    const std::size_t dollar = session_.currentFunctionName.find('$');
    if (dollar == std::string::npos) {
      return;
    }

    const std::string templateName = session_.currentFunctionName.substr(0, dollar);
    const auto family = environment_.genericFunctions.find(templateName);
    if (family == environment_.genericFunctions.end() || family->second.empty()) {
      return;
    }

    const std::vector<ast::TypeParameter>& callerTypeParams =
        genericFunctionAt(family->second.front()).typeParams;
    std::unordered_map<std::string, Type> callerBindings;
    for (std::size_t index{}; index < callerTypeParams.size() && index < callerTypeArgs->size();
         ++index) {
      callerBindings.emplace(callerTypeParams[index].name, (*callerTypeArgs)[index]);
    }

    for (const auto& calleeParam : calleeTypeParams) {
      if (bindings.contains(calleeParam.name)) {
        continue;
      }
      const auto callerBound = callerBindings.find(calleeParam.name);
      if (callerBound != callerBindings.end()) {
        bindings.emplace(calleeParam.name, callerBound->second);
      }
    }
  }

  Type TypeChecker::resolveWitnessType(SourceLocation location) const {
    const auto specialization =
        session_.functionSpecializationTypeArgs.find(session_.currentFunctionName);
    if (specialization == session_.functionSpecializationTypeArgs.end()) {
      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                          "witness-polymorphic runtime builtin requires an "
                                          "enclosing generic specialization context"));
    }

    const std::optional<Type> witness = firstNonImplTagTypeArg(specialization->second);
    if (!witness) {
      throw CompileError(formatDiagnostic(location, DiagnosticStage::TypeCheck,
                                          "witness-polymorphic runtime builtin requires an "
                                          "enclosing generic specialization context"));
    }

    return *witness;
  }

  void TypeChecker::seedUnboundTypeParamsFromCaller(
      std::unordered_map<std::string, Type>& bindings,
      const std::vector<ast::TypeParameter>& typeParams) const {
    const auto callerSpecialization =
        session_.functionSpecializationTypeArgs.find(session_.currentFunctionName);
    if (callerSpecialization == session_.functionSpecializationTypeArgs.end()) {
      return;
    }

    std::vector<Type> callerValueTypeArgs;
    callerValueTypeArgs.reserve(callerSpecialization->second.size());
    for (const Type& typeArg : callerSpecialization->second) {
      if (typeArg.kind != TypeKind::ImplTag) {
        callerValueTypeArgs.push_back(typeArg);
      }
    }

    std::size_t callerIndex{};
    for (const auto& typeParam : typeParams) {
      if (bindings.contains(typeParam.name)) {
        continue;
      }
      if (callerIndex >= callerValueTypeArgs.size()) {
        break;
      }
      bindings.emplace(typeParam.name, callerValueTypeArgs[callerIndex]);
      ++callerIndex;
    }
  }

  void TypeChecker::seedUnboundTypeParamsFromExpectedType(
      std::unordered_map<std::string, Type>& bindings, const Type& returnType,
      const std::optional<Type>& expectedType, SourceLocation location) const {
    if (!expectedType) {
      return;
    }

    unifyTypes(returnType, *expectedType, bindings, location);
  }

} // namespace noria
