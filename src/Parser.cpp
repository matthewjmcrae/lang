#include "noria/Parser.hpp"

#include "noria/Diagnostic.hpp"

#include <charconv>
#include <sstream>

namespace noria {

  namespace {
    std::string atLocation(const Token& token, std::string_view message);
  }

  // take in Tokens[] return the root of a fully built AST
  Parser::Parser(std::span<const Token> tokens) : tokens_(tokens) {}

  // parse order is module -> functions[] -> parameters[] -> block -> statements[] -> expressions[]
  ast::Module Parser::parseModule() {
    ast::Module module;

    while (peek().kind != TokenKind::End)
      module.functions.push_back(parseFunction());

    return module;
  }

  ast::Function Parser::parseFunction() {
    const Token& fnToken = expect(TokenKind::Fn, "expected function declaration");
    const Token& name = expect(TokenKind::Identifier, "expected function name");
    expect(TokenKind::LeftParen, "expected '(' after function name");
    auto parameters = parseFunctionParameters();
    expect(TokenKind::RightParen, "expected ')' after function parameters");
    expect(TokenKind::Arrow, "expected return type arrow");
    const Token& returnType = expect(TokenKind::Identifier, "expected return type");

    ast::Function function;
    function.name = name.text;
    function.parameters = std::move(parameters);
    function.returnType = returnType.text;
    function.location = fnToken.location;

    function.body = parseBlock();

    return function;
  }

  std::vector<ast::Parameter> Parser::parseFunctionParameters() {
    std::vector<ast::Parameter> parameters;

    if (peek().kind == TokenKind::RightParen) {
      return parameters;
    }

    while (true) {
      const Token& name = expect(TokenKind::Identifier, "expected parameter name");
      expect(TokenKind::Colon, "expected ':' after parameter name");
      const Token& typeName = expect(TokenKind::Identifier, "expected parameter type");

      parameters.push_back(ast::Parameter{name.text, typeName.text, name.location});

      if (!match(TokenKind::Comma)) {
        // parameters tokens are either identifiers colons typenames or commas, comma is the only
        // case left here so we can break the loop
        break;
      }
    }

    return parameters;
  }

  std::vector<std::unique_ptr<ast::Statement>> Parser::parseBlock() {
    std::vector<std::unique_ptr<ast::Statement>> statements;
    expect(TokenKind::LeftBrace, "expected block");

    while (!match(TokenKind::RightBrace)) {
      // consume statements until we hit an '}'
      if (peek().kind == TokenKind::End) {
        throw CompileError(atLocation(peek(), "unterminated function body"));
      }
      statements.push_back(parseStatement());
    }

    return statements;
  }

  std::unique_ptr<ast::Statement> Parser::parseStatement() {
    if (peek().kind == TokenKind::Return) {
      const Token& returnToken = advance();
      auto expression = parseExpression();
      expect(TokenKind::Semicolon, "expected ';' after return expression");
      return std::make_unique<ast::ReturnStatement>(std::move(expression), returnToken.location);
    }

    if (peek().kind == TokenKind::Let) {
      const Token& letToken = advance();
      const Token& name = expect(TokenKind::Identifier, "expected identifier");
      expect(TokenKind::Colon, "expected ':' after variable name");
      const Token& typeName = expect(TokenKind::Identifier, "expected variable type");
      expect(TokenKind::Equal, "expected '=' after variable type");
      auto initializer = parseExpression();
      expect(TokenKind::Semicolon, "expected ';' after variable declaration");
      return std::make_unique<ast::LetStatement>(name.text, typeName.text, std::move(initializer),
                                                 letToken.location);
    }

    const Token& lhs = peek();
    if (match(TokenKind::Identifier)) {
      expect(TokenKind::Equal, "expected '=' before variable assignment");
      auto rhs = parseExpression();
      expect(TokenKind::Semicolon, "expected ';' after assignment");
      return std::make_unique<ast::AssignmentStatement>(lhs.text, std::move(rhs), lhs.location);
    }

    if (peek().kind == TokenKind::If) {
      const Token& ifToken = advance();
      auto condition = parseExpression();
      auto thenBlock = parseBlock();
      expect(TokenKind::Else, "expected else branch");
      auto elseBlock = parseBlock();
      return std::make_unique<ast::IfStatement>(std::move(condition), std::move(thenBlock),
                                                std::move(elseBlock), ifToken.location);
    }

    if (peek().kind == TokenKind::While) {
      const Token& whileToken = advance();
      auto condition = parseExpression();
      auto body = parseBlock();
      return std::make_unique<ast::WhileStatement>(std::move(condition), std::move(body),
                                                   whileToken.location);
    }

    throw CompileError(atLocation(peek(), "expected statement"));
  }

  // call order is comparison() -> addition() -> multiplication() -> primary()
  std::unique_ptr<ast::Expression> Parser::parseExpression() {
    return parseComparison();
  }

  std::unique_ptr<ast::Expression> Parser::parseComparison() {
    auto left = parseAddition();
    while (peek().kind == TokenKind::EqualEqual || peek().kind == TokenKind::BangEqual ||
           peek().kind == TokenKind::Less || peek().kind == TokenKind::LessEqual ||
           peek().kind == TokenKind::Greater || peek().kind == TokenKind::GreaterEqual) {
      const Token& opToken = advance();
      auto right = parseAddition();

      left = std::make_unique<ast::BinaryExpression>(binaryOperatorFromToken(opToken.kind),
                                                     std::move(left), std::move(right),
                                                     opToken.location);
    }

    return left;
  }
  std::unique_ptr<ast::Expression> Parser::parseAddition() {
    auto left = parseMultiplication();

    while (peek().kind == TokenKind::Plus || peek().kind == TokenKind::Minus) {
      const Token& opToken = advance();
      auto right = parseMultiplication();

      left = std::make_unique<ast::BinaryExpression>(
          opToken.kind == TokenKind::Plus ? ast::BinaryOperator::Add
                                          : ast::BinaryOperator::Subtract,
          std::move(left), std::move(right), opToken.location);
    }

    return left;
  }

  std::unique_ptr<ast::Expression> Parser::parseMultiplication() {
    auto left = parsePrimary();

    while (peek().kind == TokenKind::Star || peek().kind == TokenKind::Slash) {
      const Token& opToken = advance();
      auto right = parsePrimary();

      left = std::make_unique<ast::BinaryExpression>(
          opToken.kind == TokenKind::Star ? ast::BinaryOperator::Multiply
                                          : ast::BinaryOperator::Divide,
          std::move(left), std::move(right), opToken.location);
    }

    return left;
  }

  // primaries are the smallest expressions in the language, which have highest prescendence so ()
  // literals and function calls
  std::unique_ptr<ast::Expression> Parser::parsePrimary() {

    //(expr)
    if (match(TokenKind::LeftParen)) {
      auto expression = parseExpression();
      expect(TokenKind::RightParen, "expected ')' after expression");
      return expression;
    }

    // function call
    if (peek().kind == TokenKind::Identifier) {
      const Token& name = advance();

      if (match(TokenKind::LeftParen)) {
        auto arguments = parseCallArguments();
        expect(TokenKind::RightParen, "expected ')' after function call arguments");
        return std::make_unique<ast::CallExpression>(name.text, std::move(arguments),
                                                     name.location);
      }
      return std::make_unique<ast::IdentifierExpression>(name.text, name.location);
    }

    if (peek().kind == TokenKind::True) {
      const Token& token = advance();
      return std::make_unique<ast::BoolLiteral>(true, token.location);
    }

    if (peek().kind == TokenKind::False) {
      const Token& token = advance();
      return std::make_unique<ast::BoolLiteral>(false, token.location);
    }

    // integer is default
    const Token& integer = expect(TokenKind::Integer, "expected expression");

    std::int64_t value = 0;
    const auto* begin = integer.text.data();
    const auto* end = integer.text.data() + integer.text.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc())
      throw CompileError(atLocation(integer, "invalid integer literal"));

    return std::make_unique<ast::IntegerLiteral>(value, integer.location);
  }

  std::vector<std::unique_ptr<ast::Expression>> Parser::parseCallArguments() {
    std::vector<std::unique_ptr<ast::Expression>> arguments;

    if (peek().kind == TokenKind::RightParen)
      return arguments;

    while (true) {
      arguments.push_back(parseExpression());
      if (!match(TokenKind::Comma))
        break;
    }

    return arguments;
  }

  // utility functions to handle traversing the input
  const Token& Parser::peek(std::size_t offset) const {
    const auto position = index_ + offset;

    if (position >= tokens_.size())
      return tokens_.back();

    return tokens_[position];
  }

  const Token& Parser::advance() {
    const Token& current = peek();
    if (current.kind != TokenKind::End)
      ++index_;
    return current;
  }

  bool Parser::match(TokenKind kind) {
    if (peek().kind != kind)
      return false;

    advance();
    return true;
  }

  const Token& Parser::expect(TokenKind kind, std::string_view message) {
    if (peek().kind == kind)
      return advance();

    std::ostringstream out;
    out << message << ", got '" << peek().text << "' (" << tokenKindName(peek().kind) << ")";
    throw CompileError(atLocation(peek(), out.str()));
  }

  ast::BinaryOperator Parser::binaryOperatorFromToken(TokenKind kind) const {
    switch (kind) {
    case TokenKind::EqualEqual:
      return ast::BinaryOperator::Equal;
    case TokenKind::BangEqual:
      return ast::BinaryOperator::NotEqual;
    case TokenKind::Less:
      return ast::BinaryOperator::Less;
    case TokenKind::LessEqual:
      return ast::BinaryOperator::LessEqual;
    case TokenKind::Greater:
      return ast::BinaryOperator::Greater;
    case TokenKind::GreaterEqual:
      return ast::BinaryOperator::GreaterEqual;
    default:
      throw CompileError("internal parser error: expected binary operator");
    }
  }

  namespace {

    std::string atLocation(const Token& token, std::string_view message) {
      std::ostringstream out;
      out << token.location.line << ":" << token.location.column << ": " << message;
      return out.str();
    }

  } // namespace
} // namespace noria
