#pragma once

#include "noria/Ast.hpp"

#include <memory>
#include <vector>

namespace noria::ast {

  std::unique_ptr<Expression> cloneExpression(const Expression& expression);
  std::unique_ptr<Statement> cloneStatement(const Statement& statement);
  std::vector<std::unique_ptr<Expression>>
  cloneExpressions(const std::vector<std::unique_ptr<Expression>>& expressions);
  std::vector<std::unique_ptr<Statement>>
  cloneStatements(const std::vector<std::unique_ptr<Statement>>& statements);

  StructLiteralField cloneStructLiteralField(const StructLiteralField& field);
  Parameter cloneParameter(const Parameter& parameter);
  TypeParameter cloneTypeParameter(const TypeParameter& typeParameter);
  StructField cloneStructField(const StructField& field);
  StructDecl cloneStructDecl(const StructDecl& structDecl);
  Function cloneFunction(const Function& function);
  ImportedName cloneImportedName(const ImportedName& importedName);
  ImportDecl cloneImportDecl(const ImportDecl& importDecl);
  Module cloneModule(const Module& module);

} // namespace noria::ast
