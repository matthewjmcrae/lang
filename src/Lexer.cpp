#include "noria/Lexer.hpp"

#include "noria/Diagnostic.hpp"

#include <cctype>
#include <sstream>
#include <unordered_map>

namespace noria {

  namespace {

    std::string atLocation(SourceLocation location, std::string_view message);

    [[noreturn]] void throwUnexpectedCharacter(SourceLocation location, char character);
  } // namespace

  Lexer::Lexer(std::string_view source) : source_(source) {}

  std::vector<Token> Lexer::lex() {
    std::vector<Token> tokens;

    while (!isAtEnd()) {
      skipWhitespace();
      if (isAtEnd())
        break;

      const auto start = location_;
      const char current = peek();

      if (std::isalpha(static_cast<unsigned char>(current)) || current == '_') {
        tokens.push_back(lexIdentifierOrKeyword());
        continue;
      }

      if (std::isdigit(static_cast<unsigned char>(current))) {
        tokens.push_back(lexInteger());
        continue;
      }

      switch (current) {
      case '-':
        if (peek(1) == '>') { // trailing return type case
          advance();
          advance();
          tokens.push_back(makeToken(TokenKind::Arrow, "->", start));
          break;
        }
        tokens.push_back(makeToken(TokenKind::Minus, std::string(1, advance()), start));
        break;
      case '(':
        tokens.push_back(makeToken(TokenKind::LeftParen, std::string(1, advance()), start));
        break;
      case ')':
        tokens.push_back(makeToken(TokenKind::RightParen, std::string(1, advance()), start));
        break;
      case '{':
        tokens.push_back(makeToken(TokenKind::LeftBrace, std::string(1, advance()), start));
        break;
      case '}':
        tokens.push_back(makeToken(TokenKind::RightBrace, std::string(1, advance()), start));
        break;
      case ';':
        tokens.push_back(makeToken(TokenKind::Semicolon, std::string(1, advance()), start));
        break;
      case ',':
        tokens.push_back(makeToken(TokenKind::Comma, std::string(1, advance()), start));
        break;
      case ':':
        tokens.push_back(makeToken(TokenKind::Colon, std::string(1, advance()), start));
        break;
      case '=':
        if (peek(1) == '=') {
          advance();
          advance();
          tokens.push_back(makeToken(TokenKind::EqualEqual, "==", start));
          break;
        }
        tokens.push_back(makeToken(TokenKind::Equal, std::string(1, advance()), start));
        break;
      case '+':
        tokens.push_back(makeToken(TokenKind::Plus, std::string(1, advance()), start));
        break;
      case '*':
        tokens.push_back(makeToken(TokenKind::Star, std::string(1, advance()), start));
        break;
      case '/':
        if (peek(1) == '/') {
          while (!isAtEnd() && peek() != '\n')
            advance();

          // skip whole line
          break;
        }
        tokens.push_back(makeToken(TokenKind::Slash, std::string(1, advance()), start));
        break;
      case '<':
        if (peek(1) == '=') {
          advance();
          advance();
          tokens.push_back(makeToken(TokenKind::LessEqual, "<=", start));
          break;
        }
        tokens.push_back(makeToken(TokenKind::Less, std::string(1, advance()), start));
        break;
      case '>':
        if (peek(1) == '=') {
          advance();
          advance();
          tokens.push_back(makeToken(TokenKind::GreaterEqual, ">=", start));
          break;
        }
        tokens.push_back(makeToken(TokenKind::Greater, std::string(1, advance()), start));
        break;
      case '!':
        if (peek(1) == '=') {
          advance();
          advance();
          tokens.push_back(makeToken(TokenKind::BangEqual, "!=", start));
          break;
        }
        throwUnexpectedCharacter(start, current);
      default:
        throwUnexpectedCharacter(start, current);
      }
    }

    tokens.push_back(makeToken(TokenKind::End, "", location_));
    return tokens;
  }

  Token Lexer::lexIdentifierOrKeyword() {
    const auto start = location_;
    const auto startIndex = index_;

    static const std::unordered_map<std::string_view, TokenKind> keywords = {
        {"fn", TokenKind::Fn},     {"return", TokenKind::Return}, {"let", TokenKind::Let},
        {"if", TokenKind::If},     {"else", TokenKind::Else},     {"while", TokenKind::While},
        {"true", TokenKind::True}, {"false", TokenKind::False},
    };

    while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_')
      advance();

    std::string text(source_.substr(startIndex, index_ - startIndex));

    if (auto it = keywords.find(text); it != keywords.end())
      return makeToken(it->second, std::move(text), start);

    return makeToken(TokenKind::Identifier, std::move(text), start);
  }

  Token Lexer::lexInteger() {
    const auto start = location_;
    const auto startIndex = index_;

    while (std::isdigit(static_cast<unsigned char>(peek())))
      advance();

    return makeToken(TokenKind::Integer,
                     std::string(source_.substr(startIndex, index_ - startIndex)), start);
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

  Token Lexer::makeToken(TokenKind kind, std::string text, SourceLocation location) const {
    return Token{kind, std::move(text), location};
  }

  // string view is ok on string literals since those live the entire lifetime of the program
  std::string_view tokenKindName(TokenKind kind) {
    switch (kind) {
    case TokenKind::End:
      return "end of file";
    case TokenKind::Identifier:
      return "identifier";
    case TokenKind::Integer:
      return "integer";
    case TokenKind::Fn:
      return "fn";
    case TokenKind::Return:
      return "return";
    case TokenKind::Let:
      return "let";
    case TokenKind::If:
      return "if";
    case TokenKind::Else:
      return "else";
    case TokenKind::While:
      return "while";
    case TokenKind::True:
      return "true";
    case TokenKind::False:
      return "false";
    case TokenKind::Arrow:
      return "->";
    case TokenKind::LeftParen:
      return "(";
    case TokenKind::RightParen:
      return ")";
    case TokenKind::LeftBrace:
      return "{";
    case TokenKind::RightBrace:
      return "}";
    case TokenKind::Semicolon:
      return ";";
    case TokenKind::Colon:
      return ":";
    case TokenKind::Comma:
      return ",";
    case TokenKind::Equal:
      return "=";
    case TokenKind::Plus:
      return "+";
    case TokenKind::Minus:
      return "-";
    case TokenKind::Star:
      return "*";
    case TokenKind::Slash:
      return "/";
    case TokenKind::EqualEqual:
      return "==";
    case TokenKind::BangEqual:
      return "!=";
    case TokenKind::Less:
      return "<";
    case TokenKind::LessEqual:
      return "<=";
    case TokenKind::Greater:
      return ">";
    case TokenKind::GreaterEqual:
      return ">=";
    case TokenKind::Unknown:
      return "unknown";
    }

    return "unknown";
  }

  namespace {

    std::string atLocation(SourceLocation location, std::string_view message) {
      std::ostringstream out;
      out << location.line << ":" << location.column << ": " << message;
      return out.str();
    }

    [[noreturn]] void throwUnexpectedCharacter(SourceLocation location, char character) {
      std::ostringstream message;
      message << "lexer: unexpected character '" << character << "'";
      throw CompileError(atLocation(location, message.str()));
    }

  } // namespace
} // namespace noria

