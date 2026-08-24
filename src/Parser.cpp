#include "noria/Parser.hpp"

#include "noria/Diagnostic.hpp"
#include "noria/SemanticTables.hpp"

#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace noria {

  namespace {

    std::optional<std::string> identifierName(const ast::Expression& expression) {
      if (const auto* identifier = dynamic_cast<const ast::IdentifierExpression*>(&expression)) {
        return identifier->name;
      }
      return std::nullopt;
    }

    std::optional<Type> builtinTypeFromName(std::string_view name) {
      using TypeFactory = Type (*)();
      static const std::unordered_map<std::string_view, TypeFactory> builtinTypes = {
          {"i32", Type::i32}, {"f64", Type::f64},         {"bool", Type::boolean},
          {"str", Type::str}, {"__rt_ptr", Type::rawPtr},
      };

      if (const auto type = builtinTypes.find(name); type != builtinTypes.end()) {
        return type->second();
      }
      return std::nullopt;
    }

    std::optional<ast::BinaryOperator> binaryOperatorForToken(TokenKind kind) {
      return binaryOperatorFromSymbol(tokenKindName(kind));
    }

    std::int64_t parseIntegerToken(const Token& integer) {
      std::int64_t value = 0;
      const auto* begin = integer.text.data();
      const auto* end = integer.text.data() + integer.text.size();
      const auto result = std::from_chars(begin, end, value);
      if (result.ec != std::errc() || result.ptr != end)
        throw CompileError(formatDiagnostic(integer.location, "invalid integer literal"));
      return value;
    }

  } // namespace

  // take in Tokens[] return the root of a fully built AST
  Parser::Parser(std::span<const Token> tokens) : tokens_(tokens) {}

  // parse order is module -> functions[]/structs[]/imports[] -> parameters[] -> block ->
  // statements[] -> expressions[]
  ast::Module Parser::parseModule() {
    ast::Module module;
    bool seenNonImportDecl = false;

    for (std::size_t index{}; index + 1 < tokens_.size(); ++index) {
      if (tokens_[index].kind == TokenKind::Struct &&
          tokens_[index + 1].kind == TokenKind::Identifier) {
        structNames_.insert(tokens_[index + 1].text);
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

    ast::FieldVisibility currentVisibility = ast::FieldVisibility::Public;
    while (!match(TokenKind::RightBrace)) {
      if (peek().kind == TokenKind::End) {
        throw CompileError(formatDiagnostic(peek().location, "unterminated struct body"));
      }

      if (match(TokenKind::Private)) {
        expect(TokenKind::Colon, "expected ':' after 'private'");
        currentVisibility = ast::FieldVisibility::Private;
        continue;
      }

      if (match(TokenKind::Public)) {
        expect(TokenKind::Colon, "expected ':' after 'public'");
        currentVisibility = ast::FieldVisibility::Public;
        continue;
      }

      TypedBinding field = parseTypedBinding("expected field name", "expected field type");
      expect(TokenKind::Semicolon, "expected ';' after field declaration");
      decl.fields.push_back(ast::StructField{field.name, std::move(field.type), field.location,
                                             currentVisibility});
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
      if (implementationTagFromName(typeParam.text)) {
        throw CompileError(
            formatDiagnostic(typeParam.location, "implementation tag '" + typeParam.text +
                                                     "' cannot be a type parameter"));
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
      typeArgs.push_back(parseTypeArgument("expected type argument"));
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

    if (peek().kind == TokenKind::Impl) {
      if (function.typeParams.empty()) {
        throw CompileError(
            formatDiagnostic(peek().location, "impl clause requires a generic function"));
      }

      expect(TokenKind::Impl, "expected 'impl' clause");
      const Token& tagToken = expect(TokenKind::Identifier, "expected implementation tag name");
      const std::optional<ImplementationTag> tag = implementationTagFromName(tagToken.text);
      if (!tag) {
        throw CompileError(formatDiagnostic(tagToken.location,
                                            "unknown implementation tag '" + tagToken.text + "'"));
      }
      function.implTag = *tag;
    }

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
      TypedBinding parameter =
          parseTypedBinding("expected parameter name", "expected parameter type");
      parameters.push_back(
          ast::Parameter{parameter.name, std::move(parameter.type), parameter.location});

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
      return parseReturnStatement();
    }

    if (peek().kind == TokenKind::Let) {
      return parseLetStatement();
    }

    if (auto localDeclaration = tryParseLocalDeclarationStatement()) {
      return localDeclaration;
    }

    if (auto assignment = tryParseAssignmentStatement()) {
      return assignment;
    }

    if (peek().kind == TokenKind::If) {
      return parseIfStatement();
    }

    if (peek().kind == TokenKind::While) {
      return parseWhileStatement();
    }

    if (isExpressionStatementStart()) {
      return parseExpressionStatement();
    }

    throw CompileError(formatDiagnostic(peek().location, "expected statement"));
  }

  std::unique_ptr<ast::Statement> Parser::parseReturnStatement() {
    const Token& returnToken = advance();
    auto expression = parseExpression();
    expect(TokenKind::Semicolon, "expected ';' after return expression");
    return std::make_unique<ast::ReturnStatement>(std::move(expression), returnToken.location);
  }

  std::unique_ptr<ast::Statement> Parser::parseLetStatement() {
    const Token& letToken = advance();

    std::string name;
    std::optional<Type> declaredType;
    std::unique_ptr<ast::Expression> initializer;

    if (peek().kind == TokenKind::Identifier && peek(1).kind == TokenKind::Equal) {
      const Token& nameToken = advance();
      name = nameToken.text;
      advance();
      initializer = parseExpression();
    } else if (isTypedBindingStart()) {
      TypedBinding binding = parseTypedBinding("expected variable name", "expected variable type");
      name = std::move(binding.name);
      declaredType = std::move(binding.type);
      if (match(TokenKind::Equal)) {
        initializer = parseExpression();
      }
    } else if (peek().kind == TokenKind::Identifier && peek(1).kind == TokenKind::Semicolon) {
      const Token& nameToken = advance();
      throw CompileError(formatDiagnostic(
          nameToken.location, "local declaration '" + nameToken.text +
                                  "' requires a type or initializer"));
    } else {
      const Token& nameToken = expect(TokenKind::Identifier, "expected identifier");
      throw CompileError(formatDiagnostic(
          nameToken.location, "local declaration '" + nameToken.text +
                                  "' requires a type or initializer"));
    }

    if (!declaredType && !initializer) {
      throw CompileError(formatDiagnostic(letToken.location,
                                          "local declaration requires a type or initializer"));
    }

    expect(TokenKind::Semicolon, "expected ';' after variable declaration");
    return std::make_unique<ast::LetStatement>(std::move(name), std::move(declaredType),
                                               std::move(initializer), letToken.location);
  }

  std::unique_ptr<ast::Statement> Parser::parseIfStatement() {
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

  std::unique_ptr<ast::Statement> Parser::parseWhileStatement() {
    const Token& whileToken = advance();
    const bool savedStructLiteralAllowed = structLiteralAllowed_;
    structLiteralAllowed_ = false;
    auto condition = parseExpression();
    structLiteralAllowed_ = savedStructLiteralAllowed;
    auto body = parseBlock();
    return std::make_unique<ast::WhileStatement>(std::move(condition), std::move(body),
                                                 whileToken.location);
  }

  std::unique_ptr<ast::Statement> Parser::parseExpressionStatement() {
    const Token& start = peek();
    auto expression = parseExpression();
    expect(TokenKind::Semicolon, "expected ';' after expression");
    return std::make_unique<ast::ExpressionStatement>(std::move(expression), start.location);
  }

  bool Parser::isExpressionStatementStart() const {
    return peek().kind == TokenKind::LeftParen || peek().kind == TokenKind::Minus ||
           peek().kind == TokenKind::Bang || peek().kind == TokenKind::Tilde ||
           peek().kind == TokenKind::True || peek().kind == TokenKind::False ||
           peek().kind == TokenKind::Integer || peek().kind == TokenKind::Float ||
           peek().kind == TokenKind::String ||
           (peek().kind == TokenKind::Identifier && peek(1).kind == TokenKind::LeftParen);
  }

  std::unique_ptr<ast::Statement> Parser::tryParseLocalDeclarationStatement() {
    if (!isTypedBindingStart()) {
      return nullptr;
    }

    const SourceLocation location = peek().location;
    TypedBinding binding = parseTypedBinding("expected variable name", "expected variable type");
    std::unique_ptr<ast::Expression> initializer;
    if (match(TokenKind::Equal)) {
      initializer = parseExpression();
    }
    expect(TokenKind::Semicolon, "expected ';' after variable declaration");
    return std::make_unique<ast::LetStatement>(
        std::move(binding.name), std::make_optional(std::move(binding.type)),
        std::move(initializer), location);
  }

  bool Parser::isTypedBindingStart() const {
    return peek().kind == TokenKind::LeftBracket ||
           (peek().kind == TokenKind::Identifier &&
            (peek(1).kind == TokenKind::Colon || peek(1).kind == TokenKind::Less));
  }

  bool Parser::isClearSimpleTypeName(std::string_view name) const {
    return builtinTypeFromName(name).has_value() || typeParamsInScope_.contains(std::string(name)) ||
           structNames_.contains(std::string(name));
  }

  bool Parser::shouldParseTypeFirstBinding() const {
    if (peek().kind == TokenKind::LeftBracket) {
      return true;
    }

    if (peek().kind != TokenKind::Identifier) {
      return false;
    }

    if (peek(1).kind == TokenKind::Less) {
      return true;
    }

    return peek(1).kind == TokenKind::Colon && isClearSimpleTypeName(peek().text);
  }

  Parser::TypedBinding Parser::parseTypedBinding(std::string_view nameMessage,
                                                 std::string_view typeMessage) {
    if (shouldParseTypeFirstBinding()) {
      Type type = parseTypeAnnotation(typeMessage);
      expect(TokenKind::Colon, "expected ':' after type");
      const Token& name = expect(TokenKind::Identifier, nameMessage);
      return TypedBinding{name.text, std::move(type), name.location};
    }

    const Token& name = expect(TokenKind::Identifier, nameMessage);
    expect(TokenKind::Colon, "expected ':' after name");
    Type type = parseTypeAnnotation(typeMessage);
    return TypedBinding{name.text, std::move(type), name.location};
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
      left = std::make_unique<ast::BinaryExpression>(binaryOperatorFromToken(opToken.kind),
                                                     std::move(left), std::move(right),
                                                     opToken.location);
    }

    return left;
  }

  std::unique_ptr<ast::Expression> Parser::parseLogicalAnd() {
    auto left = parseEquality();

    while (peek().kind == TokenKind::AmpAmp) {
      const Token& opToken = advance();
      auto right = parseEquality();
      left = std::make_unique<ast::BinaryExpression>(binaryOperatorFromToken(opToken.kind),
                                                     std::move(left), std::move(right),
                                                     opToken.location);
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
      left = std::make_unique<ast::BinaryExpression>(binaryOperatorFromToken(opToken.kind),
                                                     std::move(left), std::move(right),
                                                     opToken.location);
    }

    return left;
  }

  std::unique_ptr<ast::Expression> Parser::parseBitXor() {
    auto left = parseBitAnd();

    while (peek().kind == TokenKind::Caret) {
      const Token& opToken = advance();
      auto right = parseBitAnd();
      left = std::make_unique<ast::BinaryExpression>(binaryOperatorFromToken(opToken.kind),
                                                     std::move(left), std::move(right),
                                                     opToken.location);
    }

    return left;
  }

  std::unique_ptr<ast::Expression> Parser::parseBitAnd() {
    auto left = parseShift();

    while (peek().kind == TokenKind::Amp) {
      const Token& opToken = advance();
      auto right = parseShift();
      left = std::make_unique<ast::BinaryExpression>(binaryOperatorFromToken(opToken.kind),
                                                     std::move(left), std::move(right),
                                                     opToken.location);
    }

    return left;
  }

  std::unique_ptr<ast::Expression> Parser::parseShift() {
    auto left = parseAddition();

    while (peek().kind == TokenKind::Shl || peek().kind == TokenKind::Shr) {
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

      left = std::make_unique<ast::BinaryExpression>(binaryOperatorFromToken(opToken.kind),
                                                     std::move(left), std::move(right),
                                                     opToken.location);
    }

    return left;
  }

  std::unique_ptr<ast::Expression> Parser::parseMultiplication() {
    auto left = parseUnary();

    while (peek().kind == TokenKind::Star || peek().kind == TokenKind::Slash ||
           peek().kind == TokenKind::Percent) {
      const Token& opToken = advance();
      auto right = parseUnary();

      left = std::make_unique<ast::BinaryExpression>(binaryOperatorFromToken(opToken.kind),
                                                     std::move(left), std::move(right),
                                                     opToken.location);
    }

    return left;
  }

  std::unique_ptr<ast::Expression> Parser::parseUnary() {
    if (peek().kind == TokenKind::Bang) {
      const Token& opToken = advance();
      auto operand = parseUnary();
      return std::make_unique<ast::UnaryExpression>(*unaryOperatorFromSymbol(opToken.text),
                                                    std::move(operand),
                                                    opToken.location);
    }

    if (peek().kind == TokenKind::Minus) {
      const Token& opToken = advance();
      if (peek().kind == TokenKind::Integer) {
        const Token& integer = advance();
        const std::int64_t magnitude = parseIntegerToken(integer);
        if (magnitude > static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) + 1)
          throw CompileError(
              formatDiagnostic(integer.location, "integer literal out of i32 range"));
        return std::make_unique<ast::IntegerLiteral>(-magnitude, opToken.location);
      }

      auto operand = parseUnary();
      return std::make_unique<ast::UnaryExpression>(*unaryOperatorFromSymbol(opToken.text),
                                                    std::move(operand),
                                                    opToken.location);
    }

    if (peek().kind == TokenKind::Tilde) {
      const Token& opToken = advance();
      auto operand = parseUnary();
      return std::make_unique<ast::UnaryExpression>(*unaryOperatorFromSymbol(opToken.text),
                                                    std::move(operand),
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

    // Extension point for [expr] / .ident suffixes
    while (true) {
      if (bareCallee && !calledOnce && peek().kind == TokenKind::LeftParen) {
        advance();
        auto arguments = parseCallArguments();
        expect(TokenKind::RightParen, "expected ')' after function call arguments");

        const std::optional<std::string> calleeName = identifierName(*expression);
        if (!calleeName.has_value()) {
          throw CompileError(formatDiagnostic(expression->location, "expected expression"));
        }

        expression = std::make_unique<ast::CallExpression>(*calleeName, std::move(arguments),
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
      return parseParenthesizedExpression();
    }

    // identifier or struct literal
    if (peek().kind == TokenKind::Identifier) {
      return parseIdentifierOrStructLiteral();
    }

    if (peek().kind == TokenKind::True) {
      return parseBoolLiteral(true);
    }

    if (peek().kind == TokenKind::False) {
      return parseBoolLiteral(false);
    }

    if (peek().kind == TokenKind::Float) {
      return parseFloatLiteral();
    }

    if (peek().kind == TokenKind::String) {
      const Token& token = advance();
      return std::make_unique<ast::StringLiteral>(token.text, token.location);
    }

    if (peek().kind == TokenKind::LeftBracket) {
      return parseArrayLiteral();
    }

    if (peek().kind == TokenKind::Integer) {
      const Token& integer = advance();
      return std::make_unique<ast::IntegerLiteral>(parseIntegerToken(integer), integer.location);
    }

    throw CompileError(formatDiagnostic(peek().location, "expected expression"));
  }

  std::unique_ptr<ast::Expression> Parser::parseParenthesizedExpression() {
    const bool savedStructLiteralAllowed = structLiteralAllowed_;
    structLiteralAllowed_ = true;
    auto expression = parseExpression();
    structLiteralAllowed_ = savedStructLiteralAllowed;
    expect(TokenKind::RightParen, "expected ')' after expression");
    return expression;
  }

  std::unique_ptr<ast::Expression> Parser::parseIdentifierOrStructLiteral() {
    const Token& name = advance();

    if (auto literal = tryParseGenericStructLiteral(name)) {
      return literal;
    }

    if (structLiteralAllowed_ && peek().kind == TokenKind::LeftBrace) {
      return parseStructLiteralAfterName(name, {});
    }

    return std::make_unique<ast::IdentifierExpression>(name.text, name.location);
  }

  std::unique_ptr<ast::Expression> Parser::tryParseGenericStructLiteral(const Token& name) {
    if (!structLiteralAllowed_ || peek().kind != TokenKind::Less) {
      return nullptr;
    }

    // Generic struct literals share '<...>' syntax with comparisons, so failed type-argument
    // parsing must rewind and let the expression parser continue normally.
    const std::size_t savedIndex = index_;
    std::vector<Type> typeArgs;
    bool parsedStructLiteral = false;
    try {
      typeArgs = parseTypeArguments();
      parsedStructLiteral = peek().kind == TokenKind::LeftBrace;
    } catch (const CompileError&) {
      parsedStructLiteral = false;
    }

    if (parsedStructLiteral) {
      return parseStructLiteralAfterName(name, std::move(typeArgs));
    }

    index_ = savedIndex;
    return nullptr;
  }

  std::unique_ptr<ast::Expression>
  Parser::parseStructLiteralAfterName(const Token& name, std::vector<Type> typeArgs) {
    const SourceLocation location = peek().location;
    advance();
    std::vector<ast::StructLiteralField> fields = parseStructLiteralFields();
    expect(TokenKind::RightBrace, "expected '}' after struct literal fields");
    return std::make_unique<ast::StructLiteral>(name.text, std::move(typeArgs), std::move(fields),
                                                location);
  }

  std::vector<ast::StructLiteralField> Parser::parseStructLiteralFields() {
    std::vector<ast::StructLiteralField> fields;
    if (peek().kind == TokenKind::RightBrace) {
      return fields;
    }

    while (true) {
      const Token& fieldName = expect(TokenKind::Identifier, "expected field name");
      expect(TokenKind::Colon, "expected ':' after field name");
      auto value = parseExpression();
      fields.push_back(
          ast::StructLiteralField{fieldName.text, std::move(value), fieldName.location});

      if (!match(TokenKind::Comma))
        break;
    }

    return fields;
  }

  std::unique_ptr<ast::Expression> Parser::parseBoolLiteral(bool value) {
    const Token& token = advance();
    return std::make_unique<ast::BoolLiteral>(value, token.location);
  }

  std::unique_ptr<ast::Expression> Parser::parseFloatLiteral() {
    const Token& token = advance();
    double value = 0.0;
    try {
      value = std::stod(token.text);
    } catch (const std::invalid_argument&) {
      throw CompileError(formatDiagnostic(token.location, "invalid float literal"));
    } catch (const std::out_of_range&) {
      throw CompileError(formatDiagnostic(token.location, "float literal out of range"));
    }
    return std::make_unique<ast::FloatLiteral>(value, token.location);
  }

  std::unique_ptr<ast::Expression> Parser::parseArrayLiteral() {
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

  Type Parser::parseTypeArgument(std::string_view message) {
    if (peek().kind == TokenKind::Identifier) {
      if (const std::optional<ImplementationTag> tag = implementationTagFromName(peek().text)) {
        advance();
        return Type::implementationTag(*tag);
      }
    }

    return parseTypeAnnotation(message);
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

    if (std::optional<Type> builtinType = builtinTypeFromName(token.text)) {
      return *builtinType;
    }

    if (const std::optional<ImplementationTag> tag = implementationTagFromName(token.text)) {
      return Type::implementationTag(*tag);
    }

    if (peek().kind == TokenKind::Less) {
      std::vector<Type> typeArgs = parseTypeArguments();
      return Type::structType(token.text, std::move(typeArgs));
    }

    return Type::structType(token.text);
  }

  ast::BinaryOperator Parser::binaryOperatorFromToken(TokenKind kind) const {
    if (std::optional<ast::BinaryOperator> op = binaryOperatorForToken(kind)) {
      return *op;
    }

    throw CompileError("internal parser error: expected binary operator");
  }

} // namespace noria
