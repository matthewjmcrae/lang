#include "noria/AstPrinter.hpp"

#include "noria/AstVisitor.hpp"

#include <memory>
#include <sstream>
#include <string_view>

namespace noria {
  namespace {

    std::string_view binaryOperatorName(ast::BinaryOperator op);
    std::string_view unaryOperatorName(ast::UnaryOperator op);
    void printIndent(std::ostream& out, int indent);
    void printFunction(const ast::Function& function, std::ostream& out, int indent);
    void printBlock(std::string_view blockType,
                    const std::vector<std::unique_ptr<ast::Statement>>& statements,
                    std::ostream& out, int indent);
    std::string formatImportPath(const std::vector<std::string>& path);

    class AstPrintVisitor final : public ast::AstVisitor {
    public:
      AstPrintVisitor(std::ostream& out, int indent) : out_(out), indent_(indent) {}

      void visit(const ast::ReturnStatement& node) override {
        printIndent(out_, indent_);
        out_ << "Return\n";
        AstPrintVisitor child(out_, indent_ + 1);
        node.expression->accept(child);
      }

      void visit(const ast::LetStatement& node) override {
        printIndent(out_, indent_);
        out_ << "Let " << node.name << ": " << node.type.name() << "\n";
        AstPrintVisitor child(out_, indent_ + 1);
        node.initializer->accept(child);
      }

      void visit(const ast::AssignmentStatement& node) override {
        printIndent(out_, indent_);
        out_ << "Assign\n";
        AstPrintVisitor lhsVisitor(out_, indent_ + 1);
        node.lhs->accept(lhsVisitor);
        AstPrintVisitor rhsVisitor(out_, indent_ + 1);
        node.rhs->accept(rhsVisitor);
      }

      void visit(const ast::ExpressionStatement& node) override {
        printIndent(out_, indent_);
        out_ << "ExprStmt\n";
        AstPrintVisitor child(out_, indent_ + 1);
        node.expression->accept(child);
      }

      void visit(const ast::IfStatement& node) override {
        printIndent(out_, indent_);
        out_ << "If\n";

        printIndent(out_, indent_ + 1);
        out_ << "Condition\n";
        AstPrintVisitor conditionVisitor(out_, indent_ + 2);
        node.condition->accept(conditionVisitor);

        printBlock("Then", node.thenBranch, out_, indent_ + 1);
        if (!node.elseBranch.empty())
          printBlock("Else", node.elseBranch, out_, indent_ + 1);
      }

      void visit(const ast::WhileStatement& node) override {
        printIndent(out_, indent_);
        out_ << "While\n";

        printIndent(out_, indent_ + 1);
        out_ << "Condition\n";
        AstPrintVisitor conditionVisitor(out_, indent_ + 2);
        node.condition->accept(conditionVisitor);

        printBlock("Body", node.body, out_, indent_ + 1);
      }

      void visit(const ast::IntegerLiteral& node) override {
        printIndent(out_, indent_);
        out_ << "Integer " << node.value << "\n";
      }

      void visit(const ast::FloatLiteral& node) override {
        printIndent(out_, indent_);
        out_ << "Float " << node.value << "\n";
      }

      void visit(const ast::StringLiteral& node) override {
        printIndent(out_, indent_);
        out_ << "String \"" << node.value << "\"\n";
      }

      void visit(const ast::BoolLiteral& node) override {
        printIndent(out_, indent_);
        out_ << "Bool " << (node.value ? "true" : "false") << "\n";
      }

      void visit(const ast::IdentifierExpression& node) override {
        printIndent(out_, indent_);
        out_ << "Identifier " << node.name << "\n";
      }

      void visit(const ast::BinaryExpression& node) override {
        printIndent(out_, indent_);
        out_ << "Binary " << binaryOperatorName(node.op) << "\n";
        AstPrintVisitor child(out_, indent_ + 1);
        node.left->accept(child);
        node.right->accept(child);
      }

      void visit(const ast::UnaryExpression& node) override {
        printIndent(out_, indent_);
        out_ << "Unary " << unaryOperatorName(node.op) << "\n";
        AstPrintVisitor child(out_, indent_ + 1);
        node.operand->accept(child);
      }

      void visit(const ast::CastExpression& node) override {
        printIndent(out_, indent_);
        out_ << "Cast " << node.targetType.name() << "\n";
        AstPrintVisitor child(out_, indent_ + 1);
        node.expression->accept(child);
      }

      void visit(const ast::CallExpression& node) override {
        printIndent(out_, indent_);
        out_ << "Call " << node.callee << "\n";

        AstPrintVisitor child(out_, indent_ + 1);
        for (const auto& argument : node.arguments) {
          argument->accept(child);
        }
      }

      void visit(const ast::ArrayLiteral& node) override {
        printIndent(out_, indent_);
        out_ << "Array\n";
        AstPrintVisitor child(out_, indent_ + 1);
        for (const auto& element : node.elements) {
          element->accept(child);
        }
      }

      void visit(const ast::IndexExpression& node) override {
        printIndent(out_, indent_);
        out_ << "Index\n";
        AstPrintVisitor child(out_, indent_ + 1);
        node.base->accept(child);
        node.index->accept(child);
      }

      void visit(const ast::StructLiteral& node) override {
        printIndent(out_, indent_);
        out_ << "StructLiteral " << node.structName;
        if (!node.typeArgs.empty()) {
          out_ << "<";
          for (std::size_t index{}; index < node.typeArgs.size(); ++index) {
            if (index != 0) {
              out_ << ", ";
            }
            out_ << node.typeArgs[index].name();
          }
          out_ << ">";
        }
        out_ << "\n";
        AstPrintVisitor child(out_, indent_ + 1);
        for (const auto& field : node.fields) {
          printIndent(out_, indent_ + 1);
          out_ << "Field " << field.name << "\n";
          field.value->accept(child);
        }
      }

      void visit(const ast::FieldAccessExpression& node) override {
        printIndent(out_, indent_);
        out_ << "FieldAccess " << node.fieldName << "\n";
        AstPrintVisitor child(out_, indent_ + 1);
        node.base->accept(child);
      }

    private:
      std::ostream& out_;
      int indent_;
    };

  } // namespace

  void printAst(const ast::Module& module, std::ostream& out) {
    out << "Module\n";

    for (const auto& importDecl : module.imports) {
      printIndent(out, 1);
      out << "Import " << formatImportPath(importDecl.path) << " {";
      for (std::size_t index{}; index < importDecl.names.size(); ++index) {
        if (index != 0) {
          out << ", ";
        }
        out << importDecl.names[index].name;
      }
      out << "}\n";
    }

    for (const auto& structDecl : module.structs) {
      printIndent(out, 1);
      out << "Struct " << structDecl.name;
      if (!structDecl.typeParams.empty()) {
        out << "<";
        for (std::size_t index{}; index < structDecl.typeParams.size(); ++index) {
          if (index != 0) {
            out << ", ";
          }
          out << structDecl.typeParams[index].name;
        }
        out << ">";
      }
      out << "\n";
      for (const auto& field : structDecl.fields) {
        printIndent(out, 2);
        out << "Field " << field.name << ": " << field.type.name() << "\n";
      }
    }

    for (const auto& function : module.functions) {
      printFunction(function, out, 1);
    }
  }

  namespace {

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
      case ast::BinaryOperator::Modulo:
        return "%";
      case ast::BinaryOperator::And:
        return "&&";
      case ast::BinaryOperator::Or:
        return "||";
      case ast::BinaryOperator::BitAnd:
        return "&";
      case ast::BinaryOperator::BitOr:
        return "|";
      case ast::BinaryOperator::BitXor:
        return "^";
      case ast::BinaryOperator::Shl:
        return "<<";
      case ast::BinaryOperator::Shr:
        return ">>";
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

    std::string_view unaryOperatorName(ast::UnaryOperator op) {
      switch (op) {
      case ast::UnaryOperator::Negate:
        return "-";
      case ast::UnaryOperator::Not:
        return "!";
      case ast::UnaryOperator::BitNot:
        return "~";
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
      out << "Function " << function.name;
      if (!function.typeParams.empty()) {
        out << "<";
        for (std::size_t index{}; index < function.typeParams.size(); ++index) {
          if (index != 0) {
            out << ", ";
          }
          out << function.typeParams[index].name;
        }
        out << ">";
      }
      out << "(";

      for (std::size_t index{}; index < function.parameters.size(); ++index) {
        if (index != 0)
          out << ", ";

        out << function.parameters[index].name << ": " << function.parameters[index].type.name();
      }

      out << ") -> " << function.returnType.name() << "\n";
      printBlock("Block", function.body, out, indent + 1);
    }

    void printBlock(std::string_view blockType,
                    const std::vector<std::unique_ptr<ast::Statement>>& statements,
                    std::ostream& out, int indent) {

      printIndent(out, indent);
      out << blockType << "\n";

      for (const auto& statement : statements) {
        AstPrintVisitor visitor(out, indent + 1);
        statement->accept(visitor);
      }
    }

    std::string formatImportPath(const std::vector<std::string>& path) {
      std::ostringstream formatted;
      for (std::size_t index{}; index < path.size(); ++index) {
        if (index != 0) {
          formatted << "::";
        }
        formatted << path[index];
      }
      return formatted.str();
    }

  } // namespace

} // namespace noria
