#include "noria/AstPrinter.hpp"

#include <memory>
#include <string_view>

namespace noria {
  namespace {

    // helper function
    std::string_view binaryOperatorName(ast::BinaryOperator op);

    // private printing functions
    void printIndent(std::ostream& out, int indent);

    // print AST()->Function()->Block()->Statement()--> if/else or while -->Block() -> ...
    //                                              ->Expression() -> ...

    void printFunction(const ast::Function& function, std::ostream& out, int indent);
    void printBlock(std::string_view blockType,
                    const std::vector<std::unique_ptr<ast::Statement>>& statements,
                    std::ostream& out, int indent);
    void printStatement(const ast::Statement& statement, std::ostream& out, int indent);
    void printExpression(const ast::Expression& expression, std::ostream& out, int indent);

  } // namespace

  // all print calls follow pre-order traversal
  /*
   OUTPUT STRUCTURE:

  Module
  Function add(a: i32, b: i32) -> i32
    Block
      Return
        Binary +
          Identifier a
          Identifier b
  Function main() -> i32
    Block
      Return
        Call add
          Integer 3
          Integer 4

   */
  void printAst(const ast::Module& module, std::ostream& out) {
    out << "Module\n";

    for (const auto& function : module.functions) {
      printFunction(function, out, 1);
    }
  }

  namespace {
    // string view is fine for returning string literals
    std::string_view binaryOperatorName(ast::BinaryOperator op) {
      switch (op) {
      case ast::BinaryOperator::Add:
        return "+";
      case ast::BinaryOperator::Subtract:
        return "-";
      case ast::BinaryOperator::Multiply:
        return "*";
      case ast::BinaryOperator::Divide:
        return "/";
      case ast::BinaryOperator::Less:
        return "<";
      case ast::BinaryOperator::LessEqual:
        return "<=";
      case ast::BinaryOperator::Greater:
        return ">";
      case ast::BinaryOperator::GreaterEqual:
        return ">=";
      case ast::BinaryOperator::Equal:
        return "==";
      case ast::BinaryOperator::NotEqual:
        return "!=";
      }

      return "<unknown>";
    }

    void printIndent(std::ostream& out, int indent) {
      for (int index{}; index < indent; index++) {
        out << "  ";
      }
    }

    void printFunction(const ast::Function& function, std::ostream& out, int indent) {
      printIndent(out, indent);
      out << "Function " << function.name << "(";

      for (std::size_t index{}; index < function.parameters.size(); ++index) {
        if (index != 0)
          out << ", ";

        out << function.parameters[index].name << ": " << function.parameters[index].typeName;
      }

      out << ") -> " << function.returnType << "\n";
      printBlock("Block", function.body, out, indent + 1);
    }

    // print block type name then block contents
    void printBlock(std::string_view blockType,
                    const std::vector<std::unique_ptr<ast::Statement>>& statements,
                    std::ostream& out, int indent) {

      printIndent(out, indent);
      out << blockType << "\n";

      for (const auto& statement : statements) {
        printStatement(*statement, out, indent + 1);
      }
    }

    void printStatement(const ast::Statement& statement, std::ostream& out, int indent) {
      if (const auto* returnStatement = dynamic_cast<const ast::ReturnStatement*>(&statement)) {
        printIndent(out, indent);
        out << "Return\n";
        printExpression(*returnStatement->expression, out, indent + 1);
        return;
      }

      if (const auto* let = dynamic_cast<const ast::LetStatement*>(&statement)) {
        printIndent(out, indent);
        out << "Let " << let->name << ": " << let->typeName << "\n";
        printExpression(*let->initializer, out, indent + 1);
        return;
      }

      if (const auto* assignment = dynamic_cast<const ast::AssignmentStatement*>(&statement)) {
        printIndent(out, indent);
        out << "Assign " << assignment->lhs << "\n";
        printExpression(*assignment->rhs, out, indent + 1);
        return;
      }

      if (const auto* ifStatement = dynamic_cast<const ast::IfStatement*>(&statement)) {
        printIndent(out, indent);
        out << "If\n";

        printIndent(out, indent + 1);
        out << "Condition\n";
        printExpression(*ifStatement->condition, out, indent + 2);

        printBlock("Then", ifStatement->thenBranch, out, indent + 1);
        printBlock("Else", ifStatement->elseBranch, out, indent + 1);
        return;
      }

      if (const auto* whileStatement = dynamic_cast<const ast::WhileStatement*>(&statement)) {
        printIndent(out, indent);
        out << "While\n";

        printIndent(out, indent + 1);
        out << "Condition\n";
        printExpression(*whileStatement->condition, out, indent + 2);

        printBlock("Body", whileStatement->body, out, indent + 1);
        return;
      }

      printIndent(out, indent);
      out << "<unknown statement>\n";
    }

    void printExpression(const ast::Expression& expression, std::ostream& out, int indent) {
      if (const auto* integer = dynamic_cast<const ast::IntegerLiteral*>(&expression)) {
        printIndent(out, indent);
        out << "Integer " << integer->value << "\n";
        return;
      }

      if (const auto* boolean = dynamic_cast<const ast::BoolLiteral*>(&expression)) {
        printIndent(out, indent);
        out << "Bool " << (boolean->value ? "true" : "false") << "\n";
        return;
      }

      if (const auto* identifier = dynamic_cast<const ast::IdentifierExpression*>(&expression)) {
        printIndent(out, indent);
        out << "Identifier " << identifier->name << "\n";
        return;
      }

      if (const auto* binary = dynamic_cast<const ast::BinaryExpression*>(&expression)) {
        printIndent(out, indent);
        out << "Binary " << binaryOperatorName(binary->op) << "\n";
        printExpression(*binary->left, out, indent + 1);
        printExpression(*binary->right, out, indent + 1);
        return;
      }

      if (const auto* call = dynamic_cast<const ast::CallExpression*>(&expression)) {
        printIndent(out, indent);
        out << "Call " << call->callee << "\n";

        for (const auto& argument : call->arguments) {
          printExpression(*argument, out, indent + 1);
        }

        return;
      }

      printIndent(out, indent);
      out << "<unknown expression>\n";
    }

  } // namespace

} // namespace noria

