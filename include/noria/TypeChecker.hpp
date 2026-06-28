#pragma once

#include "noria/Ast.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace noria {

  enum class TypeKind {
    I32,
    F64,
    Bool,
    Str,
    Array,
    Struct,
    Void,
  };

  // Kind + payload type representation. Scalars (I32/F64/Bool/Str/Void) only use
  // `kind`; Array carries its element type and Struct carries its name. Later
  // phases add behaviour for the non-scalar kinds without reshaping callers.
  struct Type {
    TypeKind kind = TypeKind::I32;
    std::shared_ptr<Type> element; // Array element type
    std::string structName;        // Struct name

    Type() = default;
    explicit Type(TypeKind kind) : kind(kind) {}

    static Type i32() { return Type(TypeKind::I32); }
    static Type f64() { return Type(TypeKind::F64); }
    static Type boolean() { return Type(TypeKind::Bool); }
    static Type str() { return Type(TypeKind::Str); }
    static Type voidType() { return Type(TypeKind::Void); }
    static Type array(Type elementType);
    static Type structType(std::string name);

    bool operator==(const Type& other) const;
    bool operator!=(const Type& other) const { return !(*this == other); }

    // Human-readable name for diagnostics (e.g. "i32", "[bool]", "Point").
    std::string name() const;
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
    Type checkBuiltinCall(const ast::CallExpression& call);
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

