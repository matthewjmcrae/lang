#pragma once

#include "noria/Ast.hpp"
#include "noria/Token.hpp"
#include "noria/Types.hpp"

#include <span>

namespace noria {

  class Parser {
  public:
    explicit Parser(std::span<const Token> tokens);

    ast::Module parseModule();

  private:
    const Token& peek(std::size_t offset = 0) const;
    const Token& advance();
    bool match(TokenKind kind);
    const Token& expect(TokenKind kind, std::string_view message);

    ast::Function parseFunction();
    std::vector<ast::Parameter> parseFunctionParameters();
    std::vector<std::unique_ptr<ast::Statement>> parseBlock();
    std::unique_ptr<ast::Statement> parseStatement();
    std::unique_ptr<ast::Expression> parseExpression();
    std::unique_ptr<ast::Expression> parseLogicalOr();
    std::unique_ptr<ast::Expression> parseLogicalAnd();
    std::unique_ptr<ast::Expression> parseEquality();
    std::unique_ptr<ast::Expression> parseComparison();
    std::unique_ptr<ast::Expression> parseBitOr();
    std::unique_ptr<ast::Expression> parseBitXor();
    std::unique_ptr<ast::Expression> parseBitAnd();
    std::unique_ptr<ast::Expression> parseShift();
    std::unique_ptr<ast::Expression> parseAddition();
    std::unique_ptr<ast::Expression> parseMultiplication();
    std::unique_ptr<ast::Expression> parseUnary();
    std::unique_ptr<ast::Expression> parseCast();
    Type parseTypeAnnotation(std::string_view message);
    std::unique_ptr<ast::Expression> parsePostfix();
    std::unique_ptr<ast::Expression> parsePrimary();
    std::vector<std::unique_ptr<ast::Expression>> parseCallArguments();
    ast::BinaryOperator binaryOperatorFromToken(TokenKind kind) const;

    std::span<const Token> tokens_;
    std::size_t index_ = 0;
  };

} // namespace noria
