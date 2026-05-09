#pragma once

#include "noria/Ast.hpp"

#include <sstream>
#include <string>
#include <unordered_map>

namespace noria {

  class LlvmIrTextGenerator {
  public:
    std::string generate(const ast::Module& module) const;

  private:
    enum class IrType {
      I32,
      Bool,
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

