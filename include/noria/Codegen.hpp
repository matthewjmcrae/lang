#pragma once

#include "noria/Ast.hpp"
#include "noria/Types.hpp"

#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>

namespace noria {

  class LlvmIrTextGenerator {
  public:
    std::string generate(const ast::Module& module) const;

  private:
    struct Value {
      std::string text;
      Type type;
    };

    struct LocalBinding {
      std::string slot;
      Type type;
    };

    struct FunctionBinding {
      Type returnType;
      std::vector<Type> parameterTypes;
    };

    using Scope = std::unordered_map<std::string, LocalBinding>;

    std::string generateFunction(const ast::Function& function) const;
    bool generateStatement(const ast::Statement& statement, std::ostringstream& out,
                           int& nextTemporary, int& nextLabel, Type expectedReturnType,
                           std::vector<Scope>& scopes) const;
    bool generateStatements(const std::vector<std::unique_ptr<ast::Statement>>& statements,
                            std::ostringstream& out, int& nextTemporary, int& nextLabel,
                            Type expectedReturnType, std::vector<Scope>& scopes) const;
    std::string generateCondition(const ast::Expression& expression, std::ostringstream& out,
                                  int& nextTemporary, int& nextLabel,
                                  const std::vector<Scope>& scopes) const;
    Value generateExpression(const ast::Expression& expression, std::ostringstream& out,
                             int& nextTemporary, int& nextLabel,
                             const std::vector<Scope>& scopes) const;

    Value generateBinaryExpression(const ast::BinaryExpression& binary, std::ostringstream& out,
                                   int& nextTemporary, int& nextLabel,
                                   const std::vector<Scope>& scopes) const;
    Value generateStringLiteral(const ast::StringLiteral& literal, std::ostringstream& out,
                                int& nextTemporary) const;
    Value generateCastExpression(const ast::CastExpression& cast, std::ostringstream& out,
                                 int& nextTemporary, int& nextLabel,
                                 const std::vector<Scope>& scopes) const;
    std::optional<Value> tryGenerateBuiltinCall(const ast::CallExpression& call,
                                                std::ostringstream& out, int& nextTemporary,
                                                int& nextLabel,
                                                const std::vector<Scope>& scopes) const;

    std::string defaultIrValue(const Type& type) const;
    std::string modulePreamble() const;
    bool declareLocal(std::vector<Scope>& scopes, const std::string& name,
                      LocalBinding binding) const;
    const LocalBinding& lookupLocal(const std::vector<Scope>& scopes,
                                    const std::string& name) const;
    void collectFunctionBindings(const ast::Module& module) const;

    mutable std::unordered_map<std::string, FunctionBinding> functions_;
    mutable std::ostringstream moduleGlobals_;
    mutable int nextStringGlobal_ = 0;
  };

} // namespace noria
