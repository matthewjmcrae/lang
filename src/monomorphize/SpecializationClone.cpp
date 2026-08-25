#include "MonomorphizeInternal.hpp"

#include "../internal/AstVisitorAdapters.hpp"
#include "../internal/SpecializationSupport.hpp"
#include "noria/AstClone.hpp"
#include "noria/AstVisitor.hpp"
#include "noria/CompilerCache.hpp"
#include "noria/Diagnostic.hpp"
#include "noria/ModuleResolver.hpp"
#include "noria/SemanticTables.hpp"
#include "noria/TypeChecker.hpp"

#include <algorithm>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace noria::monomorphize_detail {

  using internal::containsUnboundTypeParam;
  using internal::findImplTag;

  // Apply specialization substitutions to an already-cloned function body.
  class SpecializationSubstitutionMutator final : public internal::RecursiveAstMutator {
  public:
    SpecializationSubstitutionMutator(std::string templateName, std::vector<Type> typeArgs,
                                      Substitution substitution)
        : templateName_(std::move(templateName)), typeArgs_(std::move(typeArgs)),
          substitution_(std::move(substitution)) {}

    void rewriteFunctionBody(ast::Function& function) { visitStatements(function.body); }

    void visit(ast::LetStatement& node) override {
      if (node.declaredType) {
        node.declaredType =
            rewriteAppliedStructType(substituteType(*node.declaredType, substitution_));
      }
      RecursiveAstMutator::visit(node);
    }

    void visit(ast::CastExpression& node) override {
      node.targetType = substituteType(node.targetType, substitution_);
      RecursiveAstMutator::visit(node);
    }

    void visit(ast::CallExpression& node) override {
      if (node.callee == templateName_) {
        node.callee = mangleSpecialization(templateName_, typeArgs_);
      }
      RecursiveAstMutator::visit(node);
    }

    void visit(ast::StructLiteral& node) override {
      for (Type& typeArg : node.typeArgs) {
        typeArg = substituteType(typeArg, substitution_);
      }
      RecursiveAstMutator::visit(node);
    }

  private:
    std::string templateName_;
    std::vector<Type> typeArgs_;
    Substitution substitution_;
  };

  // Rewrite call sites that requested concrete function specializations during type checking.
  const ast::Function* findTemplateFunction(const ast::Module& module, std::string_view name,
                                            std::optional<ImplementationTag> implTag) {
    for (const auto& function : module.functions) {
      if (function.name != name || function.typeParams.empty()) {
        continue;
      }

      if (implTag) {
        if (function.implTag && *function.implTag == *implTag) {
          return &function;
        }
        continue;
      }

      if (!function.implTag) {
        return &function;
      }
    }
    return nullptr;
  }

  const ast::StructDecl* findTemplateStruct(const ast::Module& module, std::string_view name) {
    for (const auto& structDecl : module.structs) {
      if (structDecl.name == name && !structDecl.typeParams.empty()) {
        return &structDecl;
      }
    }
    return nullptr;
  }

  ast::StructDecl cloneStructSpecialization(const ast::StructDecl& templated,
                                            const std::vector<Type>& typeArgs) {
    const Substitution substitution =
        bindTypeParameters(templated.typeParams, typeArgs, "struct specialization");

    ast::StructDecl specialized = ast::cloneStructDecl(templated);
    specialized.name = mangleSpecialization(templated.name, typeArgs);
    specialized.typeParams.clear();
    for (ast::StructField& field : specialized.fields) {
      field.type = substituteType(field.type, substitution);
    }
    return specialized;
  }

  ast::Function cloneSpecialization(const ast::Function& templated,
                                    const std::vector<Type>& typeArgs) {
    const Substitution substitution =
        bindTypeParameters(templated.typeParams, typeArgs, "specialization");

    ast::Function specialized = ast::cloneFunction(templated);
    specialized.name = mangleSpecialization(templated.name, typeArgs);
    specialized.returnType =
        rewriteAppliedStructType(substituteType(templated.returnType, substitution));
    specialized.implTag.reset();
    specialized.typeParams.clear();
    for (ast::Parameter& parameter : specialized.parameters) {
      parameter.type = rewriteAppliedStructType(substituteType(parameter.type, substitution));
    }

    SpecializationSubstitutionMutator rewriter(templated.name, typeArgs, substitution);
    rewriter.rewriteFunctionBody(specialized);
    return specialized;
  }

} // namespace noria::monomorphize_detail
