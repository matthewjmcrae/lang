#pragma once

#include "noria/Ast.hpp"
#include "noria/Builtins.hpp"
#include "noria/Types.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace noria {

  struct FunctionSignature {
    Type returnType;
    std::vector<Type> parameterTypes;
  };

  class TypeChecker {
  public:
    void check(const ast::Module& module);

  private:
    void requireKnownType(const Type& type, SourceLocation location) const;
    bool isAssignable(Type expected, Type actual) const;

    void collectFunctionSignatures(const ast::Module& module);
    void checkFunction(const ast::Function& function);
    bool checkStatements(const std::vector<std::unique_ptr<ast::Statement>>& statements,
                         Type expectedReturnType);
    bool checkStatement(const ast::Statement& statement, Type expectedReturnType);
    Type checkExpression(const ast::Expression& expression);
    Type checkBuiltinCall(const ast::CallExpression& call, const BuiltinSignature& descriptor);
    using Scope = std::unordered_map<std::string, Type>;
    void pushScope();
    void popScope();
    bool declareLocal(const std::string& name, Type type);
    Type lookupLocal(const std::string& name, SourceLocation location) const;

    std::unordered_map<std::string, FunctionSignature> functions_;
    // stack of scopes
    std::vector<Scope> scopes_;
  };

} // namespace noria
