#include "noria/Lexer.hpp"

#include "noria/Diagnostic.hpp"

#include <array>
#include <cctype>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace noria {

  namespace {

    using TwoCharacterTokens = std::unordered_map<char, TokenKind>;

    [[noreturn]] void throwUnexpectedCharacter(SourceLocation location, char character);
  } // namespace

  Lexer::Lexer(std::string_view source, std::string file)
      : source_(source), file_(std::move(file)) {
    location_.file = file_;
  }

  std::vector<Token> Lexer::lex() {
    std::vector<Token> tokens;

    while (!isAtEnd()) {
      skipWhitespace();
      if (isAtEnd())
        break;

      const char current = peek();
      if (current == '/' && peek(1) == '/') {
        skipLineComment();
        continue;
      }

      tokens.push_back(lexToken());
    }

    tokens.push_back(makeToken(TokenKind::End, "", location_));
    return tokens;
  }

  Token Lexer::lexToken() {
    const auto start = location_;
    const char current = peek();

    if (std::isalpha(static_cast<unsigned char>(current)) || current == '_') {
      return lexIdentifierOrKeyword();
    }

    if (std::isdigit(static_cast<unsigned char>(current)) ||
        (current == '.' && std::isdigit(static_cast<unsigned char>(peek(1))))) {
      return lexNumber();
    }

    if (current == '"') {
      return lexString();
    }

    if (auto token = tryLexTwoCharacterToken(start)) {
      return *token;
    }

    if (auto token = tryLexSingleCharacterToken(start)) {
      return *token;
    }

    throwUnexpectedCharacter(start, current);
  }

  std::optional<Token> Lexer::tryLexTwoCharacterToken(SourceLocation start) {
    static const std::unordered_map<char, TwoCharacterTokens> twoCharacterTokens = {
        {'-', {{'>', TokenKind::Arrow}}},
        {':', {{':', TokenKind::ColonColon}}},
        {'=', {{'=', TokenKind::EqualEqual}}},
        {'<', {{'=', TokenKind::LessEqual}, {'<', TokenKind::Shl}}},
        {'>', {{'=', TokenKind::GreaterEqual}, {'>', TokenKind::Shr}}},
        {'!', {{'=', TokenKind::BangEqual}}},
        {'&', {{'&', TokenKind::AmpAmp}}},
        {'|', {{'|', TokenKind::PipePipe}}},
    };

    const char current = peek();
    const auto first = twoCharacterTokens.find(current);
    if (first == twoCharacterTokens.end()) {
      return std::nullopt;
    }

    const char next = peek(1);
    const auto second = first->second.find(next);
    if (second == first->second.end()) {
      return std::nullopt;
    }

    advance();
    advance();
    return makeToken(second->second, std::string{current, next}, start);
  }

  std::optional<Token> Lexer::tryLexSingleCharacterToken(SourceLocation start) {
    static const std::unordered_map<char, TokenKind> singleCharacterTokens = {
        {'(', TokenKind::LeftParen},   {')', TokenKind::RightParen},
        {'[', TokenKind::LeftBracket}, {']', TokenKind::RightBracket},
        {'{', TokenKind::LeftBrace},   {'}', TokenKind::RightBrace},
        {'.', TokenKind::Dot},         {';', TokenKind::Semicolon},
        {',', TokenKind::Comma},       {':', TokenKind::Colon},
        {'=', TokenKind::Equal},       {'+', TokenKind::Plus},
        {'-', TokenKind::Minus},       {'*', TokenKind::Star},
        {'/', TokenKind::Slash},       {'<', TokenKind::Less},
        {'>', TokenKind::Greater},     {'!', TokenKind::Bang},
        {'&', TokenKind::Amp},         {'|', TokenKind::Pipe},
        {'^', TokenKind::Caret},       {'~', TokenKind::Tilde},
        {'%', TokenKind::Percent},
    };

    const char current = peek();
    const auto token = singleCharacterTokens.find(current);
    if (token == singleCharacterTokens.end()) {
      return std::nullopt;
    }

    return makeToken(token->second, std::string(1, advance()), start);
  }

  Token Lexer::lexIdentifierOrKeyword() {
    const auto start = location_;
    const auto startIndex = index_;

    static const std::unordered_map<std::string_view, TokenKind> keywords = {
        {"fn", TokenKind::Fn},           {"import", TokenKind::Import},
        {"struct", TokenKind::Struct},   {"return", TokenKind::Return},
        {"let", TokenKind::Let},         {"if", TokenKind::If},
        {"else", TokenKind::Else},       {"while", TokenKind::While},
        {"as", TokenKind::As},           {"impl", TokenKind::Impl},
        {"private", TokenKind::Private}, {"public", TokenKind::Public},
        {"true", TokenKind::True},       {"false", TokenKind::False},
    };

    while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_')
      advance();

    std::string text(source_.substr(startIndex, index_ - startIndex));

    if (auto it = keywords.find(text); it != keywords.end())
      return makeToken(it->second, std::move(text), start);

    return makeToken(TokenKind::Identifier, std::move(text), start);
  }

  Token Lexer::lexNumber() {
    const auto start = location_;
    const auto startIndex = index_;

    while (std::isdigit(static_cast<unsigned char>(peek())))
      advance();

    if (peek() == '.') {
      advance();
      while (std::isdigit(static_cast<unsigned char>(peek())))
        advance();

      const std::string text(source_.substr(startIndex, index_ - startIndex));
      return makeToken(TokenKind::Float, text, start);
    }

    return makeToken(TokenKind::Integer,
                     std::string(source_.substr(startIndex, index_ - startIndex)), start);
  }

  Token Lexer::lexString() {
    const auto start = location_;
    advance(); // opening "

    std::string value;
    while (!isAtEnd() && peek() != '"') {
      if (peek() == '\\') {
        advance();
        if (isAtEnd())
          throw CompileError(
              formatDiagnostic(location_, DiagnosticStage::Lexer, "unterminated string literal"));

        switch (peek()) {
        case 'n':
          value.push_back('\n');
          break;
        case 't':
          value.push_back('\t');
          break;
        case '"':
          value.push_back('"');
          break;
        case '\\':
          value.push_back('\\');
          break;
        default:
          throw CompileError(formatDiagnostic(location_, DiagnosticStage::Lexer,
                                              "invalid string escape sequence"));
        }
        advance();
        continue;
      }

      value.push_back(advance());
    }

    if (isAtEnd())
      throw CompileError(
          formatDiagnostic(start, DiagnosticStage::Lexer, "unterminated string literal"));

    advance(); // closing "
    return makeToken(TokenKind::String, std::move(value), start);
  }

  // helper functions
  char Lexer::peek(std::size_t offset) const {
    const auto position = index_ + offset;
    if (position >= source_.size())
      return '\0';

    return source_[position];
  }

  char Lexer::advance() {
    const char current = peek();
    ++index_;
    if (current == '\n') {
      ++location_.line;
      location_.column = 1;
    } else
      ++location_.column;

    return current;
  }

  bool Lexer::isAtEnd() const {
    return index_ >= source_.size();
  }

  void Lexer::skipWhitespace() {
    while (!isAtEnd()) {
      const char current = peek();
      if (current == ' ' || current == '\t' || current == '\r' || current == '\n') {
        advance();
        continue;
      }
      break;
    }
  }

  void Lexer::skipLineComment() {
    while (!isAtEnd() && peek() != '\n')
      advance();
  }

  Token Lexer::makeToken(TokenKind kind, std::string text, SourceLocation location) const {
    return Token{kind, std::move(text), location};
  }

  std::string_view tokenKindName(TokenKind kind) {
    static constexpr std::array<std::pair<TokenKind, std::string_view>, 52> names{{
        {TokenKind::End, "end of file"},
        {TokenKind::Identifier, "identifier"},
        {TokenKind::Integer, "integer"},
        {TokenKind::Float, "float"},
        {TokenKind::String, "string"},
        {TokenKind::Import, "import"},
        {TokenKind::Fn, "fn"},
        {TokenKind::Struct, "struct"},
        {TokenKind::Return, "return"},
        {TokenKind::Arrow, "->"},
        {TokenKind::LeftParen, "("},
        {TokenKind::RightParen, ")"},
        {TokenKind::LeftBracket, "["},
        {TokenKind::RightBracket, "]"},
        {TokenKind::LeftBrace, "{"},
        {TokenKind::RightBrace, "}"},
        {TokenKind::Semicolon, ";"},
        {TokenKind::Dot, "."},
        {TokenKind::Let, "let"},
        {TokenKind::If, "if"},
        {TokenKind::Else, "else"},
        {TokenKind::While, "while"},
        {TokenKind::True, "true"},
        {TokenKind::False, "false"},
        {TokenKind::Colon, ":"},
        {TokenKind::ColonColon, "::"},
        {TokenKind::Comma, ","},
        {TokenKind::Equal, "="},
        {TokenKind::Plus, "+"},
        {TokenKind::Minus, "-"},
        {TokenKind::Star, "*"},
        {TokenKind::Slash, "/"},
        {TokenKind::EqualEqual, "=="},
        {TokenKind::BangEqual, "!="},
        {TokenKind::Less, "<"},
        {TokenKind::LessEqual, "<="},
        {TokenKind::Greater, ">"},
        {TokenKind::GreaterEqual, ">="},
        {TokenKind::Bang, "!"},
        {TokenKind::AmpAmp, "&&"},
        {TokenKind::PipePipe, "||"},
        {TokenKind::Amp, "&"},
        {TokenKind::Pipe, "|"},
        {TokenKind::Caret, "^"},
        {TokenKind::Tilde, "~"},
        {TokenKind::Shl, "<<"},
        {TokenKind::Shr, ">>"},
        {TokenKind::Percent, "%"},
        {TokenKind::As, "as"},
        {TokenKind::Impl, "impl"},
        {TokenKind::Private, "private"},
        {TokenKind::Public, "public"},
    }};

    for (const auto& [candidate, name] : names) {
      if (candidate == kind) {
        return name;
      }
    }

    return "unknown";
  }

  namespace {

    [[noreturn]] void throwUnexpectedCharacter(SourceLocation location, char character) {
      std::ostringstream message;
      message << "unexpected character '" << character << "'";
      throw CompileError(formatDiagnostic(location, DiagnosticStage::Lexer, message.str()));
    }

  } // namespace
} // namespace noria
