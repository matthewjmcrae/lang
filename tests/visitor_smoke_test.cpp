#include "noria/Ast.hpp"
#include "noria/AstPrinter.hpp"
#include "noria/AstVisitor.hpp"
#include "noria/Types.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

namespace {

  int failures = 0;

  void expect(bool condition, const char* message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      ++failures;
    }
  }

  class CountingVisitor final : public noria::ast::AstVisitor {
  public:
    void visit(const noria::ast::IntegerLiteral&) override { ++integerLiteral_; }
    void visit(const noria::ast::FloatLiteral&) override { ++floatLiteral_; }
    void visit(const noria::ast::StringLiteral&) override { ++stringLiteral_; }
    void visit(const noria::ast::BoolLiteral&) override { ++boolLiteral_; }
    void visit(const noria::ast::UnaryExpression&) override { ++unaryExpression_; }
    void visit(const noria::ast::CastExpression&) override { ++castExpression_; }
    void visit(const noria::ast::BinaryExpression&) override { ++binaryExpression_; }
    void visit(const noria::ast::IdentifierExpression&) override { ++identifierExpression_; }
    void visit(const noria::ast::CallExpression&) override { ++callExpression_; }

    void visit(const noria::ast::ReturnStatement&) override { ++returnStatement_; }
    void visit(const noria::ast::LetStatement&) override { ++letStatement_; }
    void visit(const noria::ast::IfStatement&) override { ++ifStatement_; }
    void visit(const noria::ast::WhileStatement&) override { ++whileStatement_; }
    void visit(const noria::ast::AssignmentStatement&) override { ++assignmentStatement_; }
    void visit(const noria::ast::ExpressionStatement&) override { ++expressionStatement_; }

    void expectEachOnce() const {
      expect(integerLiteral_ == 1, "IntegerLiteral visited once");
      expect(floatLiteral_ == 1, "FloatLiteral visited once");
      expect(stringLiteral_ == 1, "StringLiteral visited once");
      expect(boolLiteral_ == 1, "BoolLiteral visited once");
      expect(unaryExpression_ == 1, "UnaryExpression visited once");
      expect(castExpression_ == 1, "CastExpression visited once");
      expect(binaryExpression_ == 1, "BinaryExpression visited once");
      expect(identifierExpression_ == 1, "IdentifierExpression visited once");
      expect(callExpression_ == 1, "CallExpression visited once");
      expect(returnStatement_ == 1, "ReturnStatement visited once");
      expect(letStatement_ == 1, "LetStatement visited once");
      expect(ifStatement_ == 1, "IfStatement visited once");
      expect(whileStatement_ == 1, "WhileStatement visited once");
      expect(assignmentStatement_ == 1, "AssignmentStatement visited once");
      expect(expressionStatement_ == 1, "ExpressionStatement visited once");
    }

  private:
    int integerLiteral_ = 0;
    int floatLiteral_ = 0;
    int stringLiteral_ = 0;
    int boolLiteral_ = 0;
    int unaryExpression_ = 0;
    int castExpression_ = 0;
    int binaryExpression_ = 0;
    int identifierExpression_ = 0;
    int callExpression_ = 0;
    int returnStatement_ = 0;
    int letStatement_ = 0;
    int ifStatement_ = 0;
    int whileStatement_ = 0;
    int assignmentStatement_ = 0;
    int expressionStatement_ = 0;
  };

  noria::ast::Module buildPrintSmokeModule() {
    using noria::SourceLocation;
    using noria::Type;
    using noria::ast::AssignmentStatement;
    using noria::ast::BinaryExpression;
    using noria::ast::BinaryOperator;
    using noria::ast::BoolLiteral;
    using noria::ast::CallExpression;
    using noria::ast::CastExpression;
    using noria::ast::ExpressionStatement;
    using noria::ast::FloatLiteral;
    using noria::ast::Function;
    using noria::ast::IdentifierExpression;
    using noria::ast::IfStatement;
    using noria::ast::IntegerLiteral;
    using noria::ast::LetStatement;
    using noria::ast::Module;
    using noria::ast::ReturnStatement;
    using noria::ast::StringLiteral;
    using noria::ast::UnaryExpression;
    using noria::ast::UnaryOperator;
    using noria::ast::WhileStatement;

    const SourceLocation loc{1, 1};

    Function function;
    function.name = "visitor_smoke";
    function.returnType = Type::i32();
    function.location = loc;

    function.body.push_back(std::make_unique<LetStatement>(
        "x", Type::i32(), std::make_unique<IntegerLiteral>(1, loc), loc));

    function.body.push_back(std::make_unique<AssignmentStatement>(
        std::make_unique<IdentifierExpression>("x", loc),
        std::make_unique<IdentifierExpression>("x", loc), loc));

    std::vector<std::unique_ptr<noria::ast::Expression>> printArguments;
    printArguments.push_back(std::make_unique<StringLiteral>("ok", loc));
    function.body.push_back(std::make_unique<ExpressionStatement>(
        std::make_unique<CallExpression>("print", std::move(printArguments), loc), loc));

    auto ifCondition =
        std::make_unique<UnaryExpression>(UnaryOperator::Not,
                                            std::make_unique<BoolLiteral>(false, loc), loc);

    std::vector<std::unique_ptr<noria::ast::Statement>> whileBody;
    whileBody.push_back(std::make_unique<AssignmentStatement>(
        std::make_unique<IdentifierExpression>("x", loc),
        std::make_unique<IntegerLiteral>(2, loc), loc));

    auto whileCondition = std::make_unique<BinaryExpression>(
        BinaryOperator::Less,
        std::make_unique<CastExpression>(
            std::make_unique<FloatLiteral>(3.0, loc), Type::i32(), loc),
        std::make_unique<IntegerLiteral>(4, loc), loc);

    std::vector<std::unique_ptr<noria::ast::Statement>> thenBranch;
    thenBranch.push_back(std::make_unique<WhileStatement>(std::move(whileCondition),
                                                          std::move(whileBody), loc));

    std::vector<std::unique_ptr<noria::ast::Statement>> elseBranch;
    elseBranch.push_back(std::make_unique<ReturnStatement>(
        std::make_unique<CastExpression>(
            std::make_unique<UnaryExpression>(
                UnaryOperator::Negate, std::make_unique<FloatLiteral>(5.0, loc), loc),
            Type::i32(), loc),
        loc));

    function.body.push_back(std::make_unique<IfStatement>(
        std::move(ifCondition), std::move(thenBranch), std::move(elseBranch), loc));

    function.body.push_back(std::make_unique<ReturnStatement>(
        std::make_unique<BinaryExpression>(
            BinaryOperator::Add, std::make_unique<IdentifierExpression>("x", loc),
            std::make_unique<IntegerLiteral>(6, loc), loc),
        loc));

    Module module;
    module.functions.push_back(std::move(function));
    return module;
  }

  void expectLabel(const std::string& output, std::string_view label, const char* message) {
    expect(output.find(label) != std::string::npos, message);
  }

} // namespace

int main() {
  using noria::SourceLocation;
  using noria::Type;
  using noria::ast::AssignmentStatement;
  using noria::ast::BinaryExpression;
  using noria::ast::BinaryOperator;
  using noria::ast::BoolLiteral;
  using noria::ast::CallExpression;
  using noria::ast::CastExpression;
  using noria::ast::ExpressionStatement;
  using noria::ast::FloatLiteral;
  using noria::ast::IdentifierExpression;
  using noria::ast::IfStatement;
  using noria::ast::IntegerLiteral;
  using noria::ast::LetStatement;
  using noria::ast::ReturnStatement;
  using noria::ast::StringLiteral;
  using noria::ast::UnaryExpression;
  using noria::ast::UnaryOperator;
  using noria::ast::WhileStatement;

  const SourceLocation loc{1, 1};
  CountingVisitor counter;

  IntegerLiteral integerLiteral(0, loc);
  FloatLiteral floatLiteral(0.0, loc);
  StringLiteral stringLiteral("s", loc);
  BoolLiteral boolLiteral(true, loc);
  IdentifierExpression identifierExpression("v", loc);
  UnaryExpression unaryExpression(UnaryOperator::Not,
                                  std::make_unique<BoolLiteral>(false, loc), loc);
  CastExpression castExpression(std::make_unique<FloatLiteral>(1.0, loc), Type::i32(), loc);
  BinaryExpression binaryExpression(
      BinaryOperator::Add, std::make_unique<IntegerLiteral>(1, loc),
      std::make_unique<IntegerLiteral>(2, loc), loc);
  std::vector<std::unique_ptr<noria::ast::Expression>> callArguments;
  callArguments.push_back(std::make_unique<StringLiteral>("a", loc));
  CallExpression callExpression("f", std::move(callArguments), loc);

  ReturnStatement returnStatement(std::make_unique<IntegerLiteral>(1, loc), loc);
  LetStatement letStatement("a", Type::i32(), std::make_unique<IntegerLiteral>(1, loc), loc);
  IfStatement ifStatement(std::make_unique<BoolLiteral>(true, loc),
                          std::vector<std::unique_ptr<noria::ast::Statement>>{},
                          std::vector<std::unique_ptr<noria::ast::Statement>>{}, loc);
  WhileStatement whileStatement(std::make_unique<BoolLiteral>(true, loc),
                                std::vector<std::unique_ptr<noria::ast::Statement>>{}, loc);
  AssignmentStatement assignmentStatement(
      std::make_unique<IdentifierExpression>("a", loc), std::make_unique<IntegerLiteral>(1, loc),
      loc);
  ExpressionStatement expressionStatement(std::make_unique<IntegerLiteral>(1, loc), loc);

  integerLiteral.accept(counter);
  floatLiteral.accept(counter);
  stringLiteral.accept(counter);
  boolLiteral.accept(counter);
  identifierExpression.accept(counter);
  unaryExpression.accept(counter);
  castExpression.accept(counter);
  binaryExpression.accept(counter);
  callExpression.accept(counter);
  returnStatement.accept(counter);
  letStatement.accept(counter);
  ifStatement.accept(counter);
  whileStatement.accept(counter);
  assignmentStatement.accept(counter);
  expressionStatement.accept(counter);

  counter.expectEachOnce();

  const noria::ast::Module module = buildPrintSmokeModule();
  std::ostringstream printed;
  noria::printAst(module, printed);
  const std::string output = printed.str();

  expectLabel(output, "Module", "printAst contains Module");
  expectLabel(output, "Function visitor_smoke", "printAst contains Function visitor_smoke");
  expectLabel(output, "Block", "printAst contains Block");
  expectLabel(output, "Let x: i32", "printAst contains Let x: i32");
  expectLabel(output, "Integer ", "printAst contains Integer");
  expectLabel(output, "Assign", "printAst contains Assign");
  expectLabel(output, "Identifier x", "printAst contains Identifier x");
  expectLabel(output, "ExprStmt", "printAst contains ExprStmt");
  expectLabel(output, "Call print", "printAst contains Call print");
  expectLabel(output, "String \"ok\"", "printAst contains String");
  expectLabel(output, "If", "printAst contains If");
  expectLabel(output, "Condition", "printAst contains Condition");
  expectLabel(output, "Unary !", "printAst contains Unary !");
  expectLabel(output, "Bool false", "printAst contains Bool false");
  expectLabel(output, "Then", "printAst contains Then");
  expectLabel(output, "While", "printAst contains While");
  expectLabel(output, "Body", "printAst contains Body");
  expectLabel(output, "Binary <", "printAst contains Binary <");
  expectLabel(output, "Cast i32", "printAst contains Cast i32");
  expectLabel(output, "Float ", "printAst contains Float");
  expectLabel(output, "Else", "printAst contains Else");
  expectLabel(output, "Return", "printAst contains Return");
  expectLabel(output, "Unary -", "printAst contains Unary -");
  expectLabel(output, "Binary +", "printAst contains Binary +");

  if (failures != 0) {
    std::cerr << failures << " visitor smoke test failure(s)\n";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
