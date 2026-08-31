#include "noria/AstVisitor.hpp"

#include "noria/Ast.hpp"

namespace noria::ast {

  void AstVisitor::visit(const ASTNode& node) {
    node.accept(*this);
  }

  void AstMutator::visit(ASTNode& node) {
    node.accept(*this);
  }

} // namespace noria::ast
