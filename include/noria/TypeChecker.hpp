#pragma once

#include "noria/Ast.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace noria {

  enum class Type {
    I32,
    Bool,
  };

  struct FunctionSignature {
    Type returnType;
    std::vector<Type> parameterTypes;
  };

  class TypeChecker {
  public:
    void check(const ast::Module& module);

  private:
    Type parseTypeName(const std::string& typeName, SourceLocation location) const;
    std::string typeName(Type type) const;
    bool isAssignable(Type expected, Type actual) const;

    void collectFunctionSignatures(const ast::Module& module);
    void checkFunction(const ast::Function& function);
    bool checkStatements(const std::vector<std::unique_ptr<ast::Statement>>& statements,
                         Type expectedReturnType);
    bool checkStatement(const ast::Statement& statement, Type expectedReturnType);
    Type checkExpression(const ast::Expression& expression);
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

