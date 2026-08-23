#include "noria/Parser.hpp"

#include "noria/AstVisitor.hpp"
#include "noria/Diagnostic.hpp"

#include <charconv>
#include <optional>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace noria {

  namespace {

    class IdentifierNameProbe final : public ast::AstVisitor {
    public:
      std::optional<std::string> name() const { return name_; }

      void visit(const ast::IdentifierExpression& node) override { name_ = node.name; }

      void visit(const ast::IntegerLiteral& node) override;
      void visit(const ast::FloatLiteral& node) override;
      void visit(const ast::StringLiteral& node) override;
      void visit(const ast::BoolLiteral& node) override;
      void visit(const ast::UnaryExpression& node) override;
      void visit(const ast::CastExpression& node) override;
      void visit(const ast::BinaryExpression& node) override;
      void visit(const ast::CallExpression& node) override;
      void visit(const ast::ArrayLiteral& node) override;
      void visit(const ast::IndexExpression& node) override;
      void visit(const ast::StructLiteral& node) override;
      void visit(const ast::FieldAccessExpression& node) override;

      void visit(const ast::ReturnStatement& node) override;
      void visit(const ast::LetStatement& node) override;
      void visit(const ast::IfStatement& node) override;
      void visit(const ast::WhileStatement& node) override;
      void visit(const ast::AssignmentStatement& node) override;
      void visit(const ast::ExpressionStatement& node) override;

    private:
      std::optional<std::string> name_;
    };

    void IdentifierNameProbe::visit(const ast::IntegerLiteral&) {}
    void IdentifierNameProbe::visit(const ast::FloatLiteral&) {}
    void IdentifierNameProbe::visit(const ast::StringLiteral&) {}
    void IdentifierNameProbe::visit(const ast::BoolLiteral&) {}
    void IdentifierNameProbe::visit(const ast::UnaryExpression&) {}
    void IdentifierNameProbe::visit(const ast::CastExpression&) {}
    void IdentifierNameProbe::visit(const ast::BinaryExpression&) {}
    void IdentifierNameProbe::visit(const ast::CallExpression&) {}
    void IdentifierNameProbe::visit(const ast::ArrayLiteral&) {}
    void IdentifierNameProbe::visit(const ast::IndexExpression&) {}
    void IdentifierNameProbe::visit(const ast::StructLiteral&) {}
    void IdentifierNameProbe::visit(const ast::FieldAccessExpression&) {}
    void IdentifierNameProbe::visit(const ast::ReturnStatement&) {}
    void IdentifierNameProbe::visit(const ast::LetStatement&) {}
    void IdentifierNameProbe::visit(const ast::IfStatement&) {}
    void IdentifierNameProbe::visit(const ast::WhileStatement&) {}
    void IdentifierNameProbe::visit(const ast::AssignmentStatement&) {}
    void IdentifierNameProbe::visit(const ast::ExpressionStatement&) {}

  } // namespace

  // take in Tokens[] return the root of a fully built AST
  Parser::Parser(std::span<const Token> tokens) : tokens_(tokens) {}

  // parse order is module -> functions[] -> parameters[] -> block -> statements[] -> expressions[]
  ast::Module Parser::parseModule() {
    ast::Module module;
    bool seenNonImportDecl = false;

    for (std::size_t index{}; index + 2 < tokens_.size(); ++index) {
      if (tokens_[index].kind == TokenKind::Struct &&
          tokens_[index + 1].kind == TokenKind::Identifier &&
          tokens_[index + 2].kind == TokenKind::Less) {
        genericStructNames_.insert(tokens_[index + 1].text);
      }
    }

    while (peek().kind != TokenKind::End) {
      if (peek().kind == TokenKind::Import) {
        if (seenNonImportDecl) {
          throw CompileError(
              formatDiagnostic(peek().location, "imports must appear before other declarations"));
        }
        module.imports.push_back(parseImportDecl());
        continue;
      }

      if (peek().kind == TokenKind::Fn) {
        seenNonImportDecl = true;
        module.functions.push_back(parseFunction());
        continue;
      }

      if (peek().kind == TokenKind::Struct) {
        seenNonImportDecl = true;
        module.structs.push_back(parseStructDecl());
        continue;
      }

      throw CompileError(
          formatDiagnostic(peek().location, "expected function or struct declaration"));
    }

    return module;
  }

  ast::ImportDecl Parser::parseImportDecl() {
    const Token& importToken = expect(TokenKind::Import, "expected import declaration");
    ast::ImportDecl decl;
    decl.location = importToken.location;

    const Token& firstSegment =
        expect(TokenKind::Identifier, "expected module path after 'import'");
    decl.path.push_back(firstSegment.text);

    while (peek().kind == TokenKind::ColonColon && peek(1).kind == TokenKind::Identifier) {
      expect(TokenKind::ColonColon, "expected module path segment after '::'");
      decl.path.push_back(
          expect(TokenKind::Identifier, "expected module path segment after '::'").text);
    }

    if (decl.path.size() < 2) {
      throw CompileError(
          formatDiagnostic(importToken.location, "expected module path after 'import'"));
    }

    expect(TokenKind::ColonColon, "expected '::' after module path");
    expect(TokenKind::LeftBrace, "expected '{' after module path");

    do {
      const Token& name = expect(TokenKind::Identifier, "expected imported name");
      decl.names.push_back(ast::ImportedName{name.text, name.location});
    } while (match(TokenKind::Comma));

    expect(TokenKind::RightBrace, "expected '}' after imported names");
    expect(TokenKind::Semicolon, "expected ';' after import declaration");

    return decl;
  }

  ast::StructDecl Parser::parseStructDecl() {
    const Token& structToken = expect(TokenKind::Struct, "expected struct declaration");
    const Token& name = expect(TokenKind::Identifier, "expected struct name");

    ast::StructDecl decl;
    decl.name = name.text;
    decl.location = structToken.location;

    if (peek().kind == TokenKind::Less) {
      decl.typeParams = parseTypeParameterList();
    }

    const std::unordered_set<std::string> savedTypeParams = std::move(typeParamsInScope_);
    typeParamsInScope_.clear();
    for (const auto& typeParam : decl.typeParams) {
      typeParamsInScope_.insert(typeParam.name);
    }

    expect(TokenKind::LeftBrace, "expected '{' after struct name");

    while (!match(TokenKind::RightBrace)) {
      if (peek().kind == TokenKind::End) {
        throw CompileError(formatDiagnostic(peek().location, "unterminated struct body"));
      }

      const Token& fieldName = expect(TokenKind::Identifier, "expected field name");
      expect(TokenKind::Colon, "expected ':' after field name");
      Type fieldType = parseTypeAnnotation("expected field type");
      expect(TokenKind::Semicolon, "expected ';' after field declaration");
      decl.fields.push_back(
          ast::StructField{fieldName.text, std::move(fieldType), fieldName.location});
    }

    typeParamsInScope_ = std::move(savedTypeParams);

    return decl;
  }

  std::vector<ast::TypeParameter> Parser::parseTypeParameterList() {
    expect(TokenKind::Less, "expected '<' before type parameters");
    if (peek().kind == TokenKind::Greater) {
      throw CompileError(formatDiagnostic(
          peek().location, "generic declaration requires at least one type parameter"));
    }

    std::vector<ast::TypeParameter> typeParams;
    std::unordered_set<std::string> seenTypeParams;
    while (true) {
      const Token& typeParam = expect(TokenKind::Identifier, "expected type parameter name");
      if (!seenTypeParams.insert(typeParam.text).second) {
        throw CompileError(formatDiagnostic(typeParam.location,
                                            "duplicate type parameter '" + typeParam.text + "'"));
      }
      typeParams.push_back(ast::TypeParameter{typeParam.text, typeParam.location});

      if (!match(TokenKind::Comma)) {
        break;
      }
    }

    expect(TokenKind::Greater, "expected '>' after type parameters");
    return typeParams;
  }

  std::vector<Type> Parser::parseTypeArguments() {
    expect(TokenKind::Less, "expected '<' before type arguments");
    if (peek().kind == TokenKind::Greater) {
      throw CompileError(formatDiagnostic(peek().location,
                                          "type application requires at least one type argument"));
    }

    std::vector<Type> typeArgs;
    while (true) {
      typeArgs.push_back(parseTypeAnnotation("expected type argument"));
      if (!match(TokenKind::Comma)) {
        break;
      }
    }

    expect(TokenKind::Greater, "expected '>' after type arguments");
    return typeArgs;
  }

  ast::Function Parser::parseFunction() {
    const Token& fnToken = expect(TokenKind::Fn, "expected function declaration");
    const Token& name = expect(TokenKind::Identifier, "expected function name");

    ast::Function function;
    function.name = name.text;
    function.location = fnToken.location;

    if (peek().kind == TokenKind::Less) {
      function.typeParams = parseTypeParameterList();
    }

    const std::unordered_set<std::string> savedTypeParams = std::move(typeParamsInScope_);
    typeParamsInScope_.clear();
    for (const auto& typeParam : function.typeParams) {
      typeParamsInScope_.insert(typeParam.name);
    }

    expect(TokenKind::LeftParen, "expected '(' after function name");
    auto parameters = parseFunctionParameters();
    expect(TokenKind::RightParen, "expected ')' after function parameters");
    expect(TokenKind::Arrow, "expected return type arrow");

    function.parameters = std::move(parameters);
    function.returnType = parseTypeAnnotation("expected return type");
    function.body = parseBlock();

    typeParamsInScope_ = std::move(savedTypeParams);

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
      parameters.push_back(
          ast::Parameter{name.text, parseTypeAnnotation("expected parameter type"), name.location});

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
        throw CompileError(formatDiagnostic(peek().location, "unterminated function body"));
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
      Type variableType = parseTypeAnnotation("expected variable type");
      expect(TokenKind::Equal, "expected '=' after variable type");
      auto initializer = parseExpression();
      expect(TokenKind::Semicolon, "expected ';' after variable declaration");
      return std::make_unique<ast::LetStatement>(name.text, std::move(variableType),
                                                 std::move(initializer), letToken.location);
    }

    if (auto assignment = tryParseAssignmentStatement()) {
      return assignment;
    }

    if (peek().kind == TokenKind::If) {
      const Token& ifToken = advance();
      const bool savedStructLiteralAllowed = structLiteralAllowed_;
      structLiteralAllowed_ = false;
      auto condition = parseExpression();
      structLiteralAllowed_ = savedStructLiteralAllowed;
      auto thenBlock = parseBlock();

      std::vector<std::unique_ptr<ast::Statement>> elseBranch;
      if (match(TokenKind::Else)) {
        if (peek().kind == TokenKind::If) {
          elseBranch.push_back(parseStatement());
        } else {
          elseBranch = parseBlock();
        }
      }

      return std::make_unique<ast::IfStatement>(std::move(condition), std::move(thenBlock),
                                                std::move(elseBranch), ifToken.location);
    }

    if (peek().kind == TokenKind::While) {
      const Token& whileToken = advance();
      const bool savedStructLiteralAllowed = structLiteralAllowed_;
      structLiteralAllowed_ = false;
      auto condition = parseExpression();
      structLiteralAllowed_ = savedStructLiteralAllowed;
      auto body = parseBlock();
      return std::make_unique<ast::WhileStatement>(std::move(condition), std::move(body),
                                                   whileToken.location);
    }

    if (peek().kind == TokenKind::LeftParen || peek().kind == TokenKind::Minus ||
        peek().kind == TokenKind::Bang || peek().kind == TokenKind::Tilde ||
        peek().kind == TokenKind::True || peek().kind == TokenKind::False ||
        peek().kind == TokenKind::Integer || peek().kind == TokenKind::Float ||
        peek().kind == TokenKind::String ||
        (peek().kind == TokenKind::Identifier && peek(1).kind == TokenKind::LeftParen)) {
      const Token& start = peek();
      auto expression = parseExpression();
      expect(TokenKind::Semicolon, "expected ';' after expression");
      return std::make_unique<ast::ExpressionStatement>(std::move(expression), start.location);
    }

    throw CompileError(formatDiagnostic(peek().location, "expected statement"));
  }

  std::unique_ptr<ast::Statement> Parser::tryParseAssignmentStatement() {
    if (peek().kind != TokenKind::Identifier) {
      return nullptr;
    }

    const TokenKind nextKind = peek(1).kind;
    if (nextKind != TokenKind::Equal && nextKind != TokenKind::LeftBracket &&
        nextKind != TokenKind::Dot && nextKind != TokenKind::LeftParen) {
      return nullptr;
    }

    const Token& start = peek();
    const std::size_t savedIndex = index_;
    auto lhs = parsePostfix();
    if (peek().kind == TokenKind::Equal) {
      advance();
      auto rhs = parseExpression();
      expect(TokenKind::Semicolon, "expected ';' after assignment");
      return std::make_unique<ast::AssignmentStatement>(std::move(lhs), std::move(rhs),
                                                        start.location);
    }

    index_ = savedIndex;
    return nullptr;
  }

  // parseLogicalOr -> parseLogicalAnd -> parseEquality -> parseComparison ->
  // parseBitOr -> parseBitXor -> parseBitAnd -> parseShift -> parseAddition ->
  // parseMultiplication -> parseUnary -> parseCast -> parsePostfix -> parsePrimary
  std::unique_ptr<ast::Expression> Parser::parseExpression() {
    return parseLogicalOr();
  }

  std::unique_ptr<ast::Expression> Parser::parseLogicalOr() {
    auto left = parseLogicalAnd();

    while (peek().kind == TokenKind::PipePipe) {
      const Token& opToken = advance();
      auto right = parseLogicalAnd();
      left = std::make_unique<ast::BinaryExpression>(ast::BinaryOperator::Or, std::move(left),
                                                     std::move(right), opToken.location);
    }

    return left;
  }

  std::unique_ptr<ast::Expression> Parser::parseLogicalAnd() {
    auto left = parseEquality();

    while (peek().kind == TokenKind::AmpAmp) {
      const Token& opToken = advance();
      auto right = parseEquality();
      left = std::make_unique<ast::BinaryExpression>(ast::BinaryOperator::And, std::move(left),
                                                     std::move(right), opToken.location);
    }

    return left;
  }

  std::unique_ptr<ast::Expression> Parser::parseEquality() {
    auto left = parseComparison();

    while (peek().kind == TokenKind::EqualEqual || peek().kind == TokenKind::BangEqual) {
      const Token& opToken = advance();
      auto right = parseComparison();
      left = std::make_unique<ast::BinaryExpression>(binaryOperatorFromToken(opToken.kind),
                                                     std::move(left), std::move(right),
                                                     opToken.location);
    }

    return left;
  }

  std::unique_ptr<ast::Expression> Parser::parseComparison() {
    auto left = parseBitOr();

    while (peek().kind == TokenKind::Less || peek().kind == TokenKind::LessEqual ||
           peek().kind == TokenKind::Greater || peek().kind == TokenKind::GreaterEqual) {
      const Token& opToken = advance();
      auto right = parseBitOr();
      left = std::make_unique<ast::BinaryExpression>(binaryOperatorFromToken(opToken.kind),
                                                     std::move(left), std::move(right),
                                                     opToken.location);
    }

    return left;
  }

  std::unique_ptr<ast::Expression> Parser::parseBitOr() {
    auto left = parseBitXor();

    while (peek().kind == TokenKind::Pipe) {
      const Token& opToken = advance();
      auto right = parseBitXor();
      left = std::make_unique<ast::BinaryExpression>(ast::BinaryOperator::BitOr, std::move(left),
                                                     std::move(right), opToken.location);
    }

    return left;
  }

  std::unique_ptr<ast::Expression> Parser::parseBitXor() {
    auto left = parseBitAnd();

    while (peek().kind == TokenKind::Caret) {
      const Token& opToken = advance();
      auto right = parseBitAnd();
      left = std::make_unique<ast::BinaryExpression>(ast::BinaryOperator::BitXor, std::move(left),
                                                     std::move(right), opToken.location);
    }

    return left;
  }

  std::unique_ptr<ast::Expression> Parser::parseBitAnd() {
    auto left = parseShift();

    while (peek().kind == TokenKind::Amp) {
      const Token& opToken = advance();
      auto right = parseShift();
      left = std::make_unique<ast::BinaryExpression>(ast::BinaryOperator::BitAnd, std::move(left),
                                                     std::move(right), opToken.location);
    }

    return left;
  }

  std::unique_ptr<ast::Expression> Parser::parseShift() {
    auto left = parseAddition();

    while (peek().kind == TokenKind::Shl || peek().kind == TokenKind::Shr) {
      const Token& opToken = advance();
      auto right = parseAddition();
      left = std::make_unique<ast::BinaryExpression>(
          opToken.kind == TokenKind::Shl ? ast::BinaryOperator::Shl : ast::BinaryOperator::Shr,
          std::move(left), std::move(right), opToken.location);
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
    auto left = parseUnary();

    while (peek().kind == TokenKind::Star || peek().kind == TokenKind::Slash ||
           peek().kind == TokenKind::Percent) {
      const Token& opToken = advance();
      auto right = parseUnary();

      ast::BinaryOperator op = ast::BinaryOperator::Multiply;
      if (opToken.kind == TokenKind::Slash)
        op = ast::BinaryOperator::Divide;
      else if (opToken.kind == TokenKind::Percent)
        op = ast::BinaryOperator::Modulo;

      left = std::make_unique<ast::BinaryExpression>(op, std::move(left), std::move(right),
                                                     opToken.location);
    }

    return left;
  }

  std::unique_ptr<ast::Expression> Parser::parseUnary() {
    if (peek().kind == TokenKind::Bang) {
      const Token& opToken = advance();
      auto operand = parseUnary();
      return std::make_unique<ast::UnaryExpression>(ast::UnaryOperator::Not, std::move(operand),
                                                    opToken.location);
    }

    if (peek().kind == TokenKind::Minus) {
      const Token& opToken = advance();
      auto operand = parseUnary();
      return std::make_unique<ast::UnaryExpression>(ast::UnaryOperator::Negate, std::move(operand),
                                                    opToken.location);
    }

    if (peek().kind == TokenKind::Tilde) {
      const Token& opToken = advance();
      auto operand = parseUnary();
      return std::make_unique<ast::UnaryExpression>(ast::UnaryOperator::BitNot, std::move(operand),
                                                    opToken.location);
    }

    return parseCast();
  }

  std::unique_ptr<ast::Expression> Parser::parseCast() {
    auto expression = parsePostfix();

    while (match(TokenKind::As)) {
      const SourceLocation typeLocation = peek().location;
      Type targetType = parseTypeAnnotation("expected cast target type");
      expression = std::make_unique<ast::CastExpression>(std::move(expression),
                                                         std::move(targetType), typeLocation);
    }

    return expression;
  }

  std::unique_ptr<ast::Expression> Parser::parsePostfix() {
    const bool bareCallee = peek().kind == TokenKind::Identifier;
    auto expression = parsePrimary();
    bool calledOnce = false;

    // Extension point for [expr] / .ident suffixes (Phase 3+).
    while (true) {
      if (bareCallee && !calledOnce && peek().kind == TokenKind::LeftParen) {
        advance();
        auto arguments = parseCallArguments();
        expect(TokenKind::RightParen, "expected ')' after function call arguments");

        IdentifierNameProbe probe;
        expression->accept(probe);
        if (!probe.name().has_value()) {
          throw CompileError(formatDiagnostic(expression->location, "expected expression"));
        }

        expression = std::make_unique<ast::CallExpression>(*probe.name(), std::move(arguments),
                                                           expression->location);
        calledOnce = true;
        continue;
      }

      if (peek().kind == TokenKind::LeftBracket) {
        const SourceLocation location = advance().location;
        auto index = parseExpression();
        expect(TokenKind::RightBracket, "expected ']' after index expression");
        expression = std::make_unique<ast::IndexExpression>(std::move(expression), std::move(index),
                                                            location);
        continue;
      }

      if (peek().kind == TokenKind::Dot) {
        const SourceLocation location = advance().location;
        const Token& field = expect(TokenKind::Identifier, "expected field name after '.'");
        expression = std::make_unique<ast::FieldAccessExpression>(std::move(expression), field.text,
                                                                  location);
        continue;
      }

      break;
    }

    return expression;
  }

  // primaries are the smallest expressions in the language, which have highest prescendence so ()
  // literals and function calls
  std::unique_ptr<ast::Expression> Parser::parsePrimary() {

    //(expr)
    if (match(TokenKind::LeftParen)) {
      const bool savedStructLiteralAllowed = structLiteralAllowed_;
      structLiteralAllowed_ = true;
      auto expression = parseExpression();
      structLiteralAllowed_ = savedStructLiteralAllowed;
      expect(TokenKind::RightParen, "expected ')' after expression");
      return expression;
    }

    // identifier or struct literal
    if (peek().kind == TokenKind::Identifier) {
      const Token& name = advance();

      if (structLiteralAllowed_ && genericStructNames_.contains(name.text) &&
          peek().kind == TokenKind::Less) {
        std::vector<Type> typeArgs = parseTypeArguments();
        const SourceLocation location = peek().location;
        expect(TokenKind::LeftBrace, "expected '{' after generic struct type arguments");

        std::vector<ast::StructLiteralField> fields;
        if (peek().kind != TokenKind::RightBrace) {
          while (true) {
            const Token& fieldName = expect(TokenKind::Identifier, "expected field name");
            expect(TokenKind::Colon, "expected ':' after field name");
            auto value = parseExpression();
            fields.push_back(
                ast::StructLiteralField{fieldName.text, std::move(value), fieldName.location});

            if (!match(TokenKind::Comma))
              break;
          }
        }

        expect(TokenKind::RightBrace, "expected '}' after struct literal fields");
        return std::make_unique<ast::StructLiteral>(name.text, std::move(typeArgs),
                                                    std::move(fields), location);
      }

      if (structLiteralAllowed_ && peek().kind == TokenKind::LeftBrace) {
        const SourceLocation location = peek().location;
        advance();

        std::vector<ast::StructLiteralField> fields;
        if (peek().kind != TokenKind::RightBrace) {
          while (true) {
            const Token& fieldName = expect(TokenKind::Identifier, "expected field name");
            expect(TokenKind::Colon, "expected ':' after field name");
            auto value = parseExpression();
            fields.push_back(
                ast::StructLiteralField{fieldName.text, std::move(value), fieldName.location});

            if (!match(TokenKind::Comma))
              break;
          }
        }

        expect(TokenKind::RightBrace, "expected '}' after struct literal fields");
        return std::make_unique<ast::StructLiteral>(name.text, std::vector<Type>{},
                                                    std::move(fields), location);
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

    if (peek().kind == TokenKind::Float) {
      const Token& token = advance();
      double value = std::stod(token.text);
      return std::make_unique<ast::FloatLiteral>(value, token.location);
    }

    if (peek().kind == TokenKind::String) {
      const Token& token = advance();
      return std::make_unique<ast::StringLiteral>(token.text, token.location);
    }

    if (peek().kind == TokenKind::LeftBracket) {
      const SourceLocation location = advance().location;
      std::vector<std::unique_ptr<ast::Expression>> elements;

      if (peek().kind != TokenKind::RightBracket) {
        while (true) {
          elements.push_back(parseExpression());
          if (!match(TokenKind::Comma))
            break;
        }
      }

      expect(TokenKind::RightBracket, "expected ']' after array literal elements");
      return std::make_unique<ast::ArrayLiteral>(std::move(elements), location);
    }

    if (peek().kind == TokenKind::Integer) {
      const Token& integer = advance();

      std::int64_t value = 0;
      const auto* begin = integer.text.data();
      const auto* end = integer.text.data() + integer.text.size();
      const auto result = std::from_chars(begin, end, value);
      if (result.ec != std::errc())
        throw CompileError(formatDiagnostic(integer.location, "invalid integer literal"));

      return std::make_unique<ast::IntegerLiteral>(value, integer.location);
    }

    throw CompileError(formatDiagnostic(peek().location, "expected expression"));
  }

  std::vector<std::unique_ptr<ast::Expression>> Parser::parseCallArguments() {
    std::vector<std::unique_ptr<ast::Expression>> arguments;
    const bool savedStructLiteralAllowed = structLiteralAllowed_;
    structLiteralAllowed_ = true;

    if (peek().kind == TokenKind::RightParen) {
      structLiteralAllowed_ = savedStructLiteralAllowed;
      return arguments;
    }

    while (true) {
      arguments.push_back(parseExpression());
      if (!match(TokenKind::Comma))
        break;
    }

    structLiteralAllowed_ = savedStructLiteralAllowed;
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
    throw CompileError(formatDiagnostic(peek().location, out.str()));
  }

  Type Parser::parseTypeAnnotation(std::string_view message) {
    if (peek().kind == TokenKind::LeftBracket) {
      advance();
      Type elementType = parseTypeAnnotation("expected array element type");
      expect(TokenKind::RightBracket, "expected ']' after array element type");
      return Type::array(std::move(elementType));
    }

    const Token& token = expect(TokenKind::Identifier, message);

    if (typeParamsInScope_.contains(token.text)) {
      return Type::typeParam(token.text);
    }

    if (token.text == "i32")
      return Type::i32();
    if (token.text == "f64")
      return Type::f64();
    if (token.text == "bool")
      return Type::boolean();
    if (token.text == "str")
      return Type::str();

    if (peek().kind == TokenKind::Less) {
      std::vector<Type> typeArgs = parseTypeArguments();
      return Type::structType(token.text, std::move(typeArgs));
    }

    return Type::structType(token.text);
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

} // namespace noria
