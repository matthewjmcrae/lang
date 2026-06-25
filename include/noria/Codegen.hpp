#pragma once

#include "noria/Ast.hpp"

#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>

namespace noria {

  class LlvmIrTextGenerator {
  public:
    std::string generate(const ast::Module& module) const;

  private:
    enum class IrTypeKind {
      I32,
      F64,
      Bool,
      Str,
      Array,
      Struct,
      Void,
    };

    // Codegen mirror of TypeChecker's Type: kind plus optional payload so the
    // emitter knows aggregate layouts (Array element / Struct name) for later
    // getelementptr work. Scalars only use `kind`.
    struct IrType {
      IrTypeKind kind;
      std::shared_ptr<IrType> element; // Array element type
      std::string structName;          // Struct name

      IrType() = default;
      explicit IrType(IrTypeKind kind) : kind(kind) {}

      static IrType i32() { return IrType(IrTypeKind::I32); }
      static IrType f64() { return IrType(IrTypeKind::F64); }
      static IrType boolean() { return IrType(IrTypeKind::Bool); }
      static IrType str() { return IrType(IrTypeKind::Str); }
      static IrType voidType() { return IrType(IrTypeKind::Void); }
      static IrType array(IrType elementType);
      static IrType structType(std::string name);

      bool operator==(const IrType& other) const;
      bool operator!=(const IrType& other) const { return !(*this == other); }
    };

    struct Value {
      std::string text;
      IrType type;
    };

    struct LocalBinding {
      std::string slot;
      IrType type;
    };

    struct FunctionBinding {
      IrType returnType;
      std::vector<IrType> parameterTypes;
    };

    using Scope = std::unordered_map<std::string, LocalBinding>;

    std::string generateFunction(const ast::Function& function) const;
    bool generateStatement(const ast::Statement& statement, std::ostringstream& out,
                           int& nextTemporary, int& nextLabel, IrType expectedReturnType,
                           std::vector<Scope>& scopes) const;
    bool generateStatements(const std::vector<std::unique_ptr<ast::Statement>>& statements,
                            std::ostringstream& out, int& nextTemporary, int& nextLabel,
                            IrType expectedReturnType, std::vector<Scope>& scopes) const;
    std::string generateCondition(const ast::Expression& expression, std::ostringstream& out,
                                  int& nextTemporary, const std::vector<Scope>& scopes) const;
    Value generateExpression(const ast::Expression& expression, std::ostringstream& out,
                             int& nextTemporary, const std::vector<Scope>& scopes) const;

    Value generateBinaryExpression(const ast::BinaryExpression& binary, std::ostringstream& out,
                                   int& nextTemporary, const std::vector<Scope>& scopes) const;

    IrType parseIrType(const std::string& typeName) const;
    std::string llvmType(IrType type) const;
    bool declareLocal(std::vector<Scope>& scopes, const std::string& name,
                      LocalBinding binding) const;
    const LocalBinding& lookupLocal(const std::vector<Scope>& scopes,
                                    const std::string& name) const;
    void collectFunctionBindings(const ast::Module& module) const;

    mutable std::unordered_map<std::string, FunctionBinding> functions_;
  };

} // namespace noria

