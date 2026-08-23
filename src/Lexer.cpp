#include "noria/Lexer.hpp"

#include "noria/Diagnostic.hpp"

#include <cctype>
#include <sstream>
#include <unordered_map>

namespace noria {

  namespace {

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

      if (std::isdigit(static_cast<unsigned char>(current)) ||
          (current == '.' && std::isdigit(static_cast<unsigned char>(peek(1))))) {
        tokens.push_back(lexNumber());
        continue;
      }

      if (current == '"') {
        tokens.push_back(lexString());
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
      case '[':
        tokens.push_back(makeToken(TokenKind::LeftBracket, std::string(1, advance()), start));
        break;
      case ']':
        tokens.push_back(makeToken(TokenKind::RightBracket, std::string(1, advance()), start));
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
        if (peek(1) == '<') {
          advance();
          advance();
          tokens.push_back(makeToken(TokenKind::Shl, "<<", start));
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
        if (peek(1) == '>') {
          advance();
          advance();
          tokens.push_back(makeToken(TokenKind::Shr, ">>", start));
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
        tokens.push_back(makeToken(TokenKind::Bang, std::string(1, advance()), start));
        break;
      case '&':
        if (peek(1) == '&') {
          advance();
          advance();
          tokens.push_back(makeToken(TokenKind::AmpAmp, "&&", start));
          break;
        }
        tokens.push_back(makeToken(TokenKind::Amp, std::string(1, advance()), start));
        break;
      case '|':
        if (peek(1) == '|') {
          advance();
          advance();
          tokens.push_back(makeToken(TokenKind::PipePipe, "||", start));
          break;
        }
        tokens.push_back(makeToken(TokenKind::Pipe, std::string(1, advance()), start));
        break;
      case '^':
        tokens.push_back(makeToken(TokenKind::Caret, std::string(1, advance()), start));
        break;
      case '~':
        tokens.push_back(makeToken(TokenKind::Tilde, std::string(1, advance()), start));
        break;
      case '%':
        tokens.push_back(makeToken(TokenKind::Percent, std::string(1, advance()), start));
        break;
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
        {"fn", TokenKind::Fn}, {"return", TokenKind::Return}, {"let", TokenKind::Let},
        {"if", TokenKind::If}, {"else", TokenKind::Else},     {"while", TokenKind::While},
        {"as", TokenKind::As}, {"true", TokenKind::True},     {"false", TokenKind::False},
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
    case TokenKind::Float:
      return "float";
    case TokenKind::String:
      return "string";
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
    case TokenKind::LeftBracket:
      return "[";
    case TokenKind::RightBracket:
      return "]";
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
    case TokenKind::Bang:
      return "!";
    case TokenKind::AmpAmp:
      return "&&";
    case TokenKind::PipePipe:
      return "||";
    case TokenKind::Amp:
      return "&";
    case TokenKind::Pipe:
      return "|";
    case TokenKind::Caret:
      return "^";
    case TokenKind::Tilde:
      return "~";
    case TokenKind::Shl:
      return "<<";
    case TokenKind::Shr:
      return ">>";
    case TokenKind::Percent:
      return "%";
    case TokenKind::As:
      return "as";
    case TokenKind::Unknown:
      return "unknown";
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
