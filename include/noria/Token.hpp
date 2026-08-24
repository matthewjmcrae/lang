#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace noria {

  enum class TokenKind {
    End,
    Identifier,
    Integer,
    Float,
    String,
    Import,
    Fn,
    Struct,
    Return,
    Arrow,
    LeftParen,
    RightParen,
    LeftBracket,
    RightBracket,
    LeftBrace,
    RightBrace,
    Semicolon,
    Dot,
    Let,
    If,
    Else,
    While,
    True,
    False,
    Colon,
    ColonColon,
    Comma,
    Equal,
    Plus,
    Minus,
    Star,
    Slash,
    EqualEqual,
    BangEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Bang,
    AmpAmp,
    PipePipe,
    Amp,
    Pipe,
    Caret,
    Tilde,
    Shl,
    Shr,
    Percent,
    As,
    Impl,
    Unknown,
  };

  struct SourceLocation {
    std::string file;
    std::size_t line = 1;
    std::size_t column = 1;
  };

  struct Token {
    TokenKind kind = TokenKind::Unknown;
    std::string text;
    SourceLocation location;
  };

  std::string_view tokenKindName(TokenKind kind);

} // namespace noria
