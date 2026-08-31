#include "MonomorphizeInternal.hpp"

#include "noria/HashTable.hpp"

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

  bool locationsMatch(const SourceLocation& left, const SourceLocation& right) {
    if (left.file != right.file) {
      return false;
    }
    return left.line == right.line && left.column == right.column;
  }

  class CallSiteMutator final : public internal::RecursiveAstMutator {
  public:
    explicit CallSiteMutator(const std::vector<SpecializationRequest>& requests)
        : requests_(requests), matched_(requests.size(), false) {}

    void rewriteFunction(ast::Function& function) {
      currentFunction_ = function.name;
      visitStatements(function.body);
    }

    bool allMatched() const {
      return std::all_of(matched_.begin(), matched_.end(), [](bool matched) { return matched; });
    }

    const std::vector<bool>& matched() const { return matched_; }

    void visit(ast::CallExpression& node) override {
      const std::string originalCallee = node.callee;
      std::optional<std::string> selectedCallee;
      for (std::size_t index{}; index < requests_.size(); ++index) {
        const SpecializationRequest& request = requests_[index];
        if (locationsMatch(node.location, request.callSiteLocation) &&
            originalCallee == request.templateName &&
            currentFunction_ == request.enclosingFunction) {
          const std::string mangled = mangleSpecialization(request.templateName, request.typeArgs);
          if (selectedCallee && *selectedCallee != mangled) {
            throw CompileError("monomorphize: internal error: ambiguous generic call rewrite");
          }
          selectedCallee = mangled;
          matched_[index] = true;
        }
      }
      if (selectedCallee) {
        node.callee = std::move(*selectedCallee);
      }
      RecursiveAstMutator::visit(node);
    }

  private:
    const std::vector<SpecializationRequest>& requests_;
    std::vector<bool> matched_;
    std::string currentFunction_;
  };

  // Rewrite concrete generic struct applications after their specialized structs are emitted.
  class StructApplicationMutator final : public internal::RecursiveAstMutator {
  public:
    explicit StructApplicationMutator(const std::vector<StructSpecializationRequest>& requests)
        : requestsByFunction_(groupByEnclosingFunction(requests)) {}

    void rewriteFunction(ast::Function& function) {
      currentFunction_ = function.name;
      visitStatements(function.body);
    }

    void rewriteModule(ast::Module& module) {
      for (auto& structDecl : module.structs) {
        if (!structDecl.typeParams.empty()) {
          continue;
        }

        for (auto& field : structDecl.fields) {
          field.type = rewriteAppliedStructType(field.type);
        }
      }

      for (auto& function : module.functions) {
        if (!function.typeParams.empty()) {
          continue;
        }

        if (!function.returnType) {
          throw CompileError("monomorphize: function '" + function.name +
                             "' has an unresolved return type");
        }
        function.returnType = rewriteAppliedStructType(*function.returnType);
        for (auto& parameter : function.parameters) {
          parameter.type = rewriteAppliedStructType(parameter.type);
        }
        rewriteFunction(function);
      }
    }

    void visit(ast::LetStatement& node) override {
      if (node.declaredType) {
        node.declaredType = rewriteAppliedStructType(*node.declaredType);
      }
      RecursiveAstMutator::visit(node);
    }

    void visit(ast::CastExpression& node) override {
      node.targetType = rewriteAppliedStructType(node.targetType);
      RecursiveAstMutator::visit(node);
    }
    void visit(ast::StructLiteral& node) override {
      if (!node.typeArgs.empty()) {
        if (!containsUnboundTypeParam(Type::structType(node.structName, node.typeArgs))) {
          node.structName = mangleSpecialization(node.structName, node.typeArgs);
          node.typeArgs.clear();
        }
        RecursiveAstMutator::visit(node);
        return;
      }

      const auto requests = requestsByFunction_.find(currentFunction_);
      if (requests != requestsByFunction_.end()) {
        for (const auto& request : requests->second) {
          if (locationsMatch(node.location, request.useSiteLocation) &&
              node.structName == request.templateName) {
            node.structName = mangleSpecialization(request.templateName, request.typeArgs);
            break;
          }
        }
      }

      RecursiveAstMutator::visit(node);
    }

  private:
    static std::unordered_map<std::string, std::vector<StructSpecializationRequest>>
    groupByEnclosingFunction(const std::vector<StructSpecializationRequest>& requests) {
      std::unordered_map<std::string, std::vector<StructSpecializationRequest>> grouped;
      for (const StructSpecializationRequest& request : requests) {
        grouped[request.enclosingFunction].push_back(request);
      }
      return grouped;
    }

    std::unordered_map<std::string, std::vector<StructSpecializationRequest>> requestsByFunction_;
    std::string currentFunction_;
  };

  void rewriteStructApplications(ast::Module& module,
                                 const std::vector<StructSpecializationRequest>& requests) {
    StructApplicationMutator rewriter(requests);
    rewriter.rewriteModule(module);
  }

  std::string locationKey(const SourceLocation& location) {
    std::ostringstream out;
    out << location.file << ':' << location.line << ':' << location.column;
    return out.str();
  }

  std::vector<SpecializationRequest>
  dedupeFunctionRewriteRequests(const std::vector<SpecializationRequest>& requests) {
    std::vector<SpecializationRequest> unique;
    std::unordered_set<std::string> seen;
    unique.reserve(requests.size());
    for (const SpecializationRequest& request : requests) {
      if (!request.rewriteCallSite) {
        continue;
      }
      const std::string key = request.enclosingFunction + "|" + request.templateName + "|" +
                              mangleSpecialization(request.templateName, request.typeArgs) + "|" +
                              locationKey(request.callSiteLocation);
      if (!seen.insert(key).second) {
        continue;
      }
      unique.push_back(request);
    }
    return unique;
  }

  HashTable<std::string, std::size_t>
  concreteFunctionIndexByName(const ast::Module& module) {
    HashTable<std::string, std::size_t> indexes;
    indexes.reserve(module.functions.size());
    for (std::size_t index{}; index < module.functions.size(); ++index) {
      const ast::Function& function = module.functions[index];
      if (function.typeParams.empty()) {
        indexes.emplace(function.name, index);
      }
    }
    return indexes;
  }

  std::unordered_map<std::string, std::vector<SpecializationRequest>>
  groupFunctionRequestsByEnclosingFunction(const std::vector<SpecializationRequest>& requests) {
    std::unordered_map<std::string, std::vector<SpecializationRequest>> grouped;
    for (const SpecializationRequest& request : requests) {
      if (!request.rewriteCallSite) {
        continue;
      }
      if (request.enclosingFunction.empty()) {
        throw CompileError(
            "monomorphize: internal error: generic function request has no enclosing function");
      }
      grouped[request.enclosingFunction].push_back(request);
    }
    return grouped;
  }

  [[noreturn]] void throwMissingRewrite(const SpecializationRequest& request) {
    throw CompileError("monomorphize: internal error: recorded call to generic function '" +
                       request.templateName + "' in '" + request.enclosingFunction + "' at " +
                       locationKey(request.callSiteLocation) + " was not found");
  }

  void rewriteTargetedGenericCallSites(ast::Module& module,
                                       const std::vector<SpecializationRequest>& requests) {
    const std::vector<SpecializationRequest> unique = dedupeFunctionRewriteRequests(requests);
    const auto grouped = groupFunctionRequestsByEnclosingFunction(unique);
    const auto functionIndexes = concreteFunctionIndexByName(module);

    for (const auto& [functionName, functionRequests] : grouped) {
      const auto functionIndex = functionIndexes.find(functionName);
      if (functionIndex == functionIndexes.end()) {
        throwMissingRewrite(functionRequests.front());
      }

      CallSiteMutator rewriter(functionRequests);
      rewriter.rewriteFunction(module.functions.at(functionIndex->second));
      if (!rewriter.allMatched()) {
        const std::vector<bool>& matched = rewriter.matched();
        for (std::size_t index{}; index < matched.size(); ++index) {
          if (!matched[index]) {
            throwMissingRewrite(functionRequests[index]);
          }
        }
      }
    }
  }

} // namespace noria::monomorphize_detail

namespace noria {

  using namespace monomorphize_detail;

  void rewriteGenericCallSites(ast::Module& module,
                               const std::vector<SpecializationRequest>& requests) {
    CallSiteMutator rewriter(requests);
    for (auto& function : module.functions) {
      rewriter.rewriteFunction(function);
    }
  }

} // namespace noria
