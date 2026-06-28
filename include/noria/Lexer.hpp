#pragma once

#include "noria/Token.hpp"

#include <string_view>
#include <vector>

namespace noria {

  class Lexer {
  public:
    explicit Lexer(std::string_view source);

    std::vector<Token> lex();

  private:
    char peek(std::size_t offset = 0) const;
    char advance();
    bool isAtEnd() const;
    void skipWhitespace();
    Token makeToken(TokenKind kind, std::string text, SourceLocation location) const;
    Token lexIdentifierOrKeyword();
    Token lexNumber();
    Token lexString();

    std::string_view source_;
    std::size_t index_ = 0;
    SourceLocation location_;
  };

} // namespace noria
