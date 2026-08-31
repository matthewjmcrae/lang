#pragma once

#include "noria/Ast.hpp"
#include "noria/Token.hpp"
#include "noria/Types.hpp"

#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace noria {

  class Parser {
  public:
    explicit Parser(std::span<const Token> tokens);

    ast::Module parseModule();

  private:
    struct TypedBinding {
      std::string name;
      Type type;
      SourceLocation location;
    };

    const Token& peek(std::size_t offset = 0) const;
    const Token& advance();
    bool match(TokenKind kind);
    const Token& expect(TokenKind kind, std::string_view message);

    ast::Function parseFunction();
    ast::StructDecl parseStructDecl();
    ast::ImportDecl parseImportDecl();
    std::vector<ast::TypeParameter> parseTypeParameterList();
    std::vector<Type> parseTypeArguments();
    std::vector<ast::Parameter> parseFunctionParameters();
    std::vector<std::unique_ptr<ast::Statement>> parseBlock();
    std::unique_ptr<ast::Statement> parseStatement();
    std::unique_ptr<ast::Statement> parseReturnStatement();
    std::unique_ptr<ast::Statement> parseLetStatement();
    std::unique_ptr<ast::Statement> parseIfStatement();
    std::unique_ptr<ast::Statement> parseWhileStatement();
    std::unique_ptr<ast::Statement> parseExpressionStatement();
    std::unique_ptr<ast::Statement> tryParseLocalDeclarationStatement();
    std::unique_ptr<ast::Statement> tryParseAssignmentStatement();
    bool isExpressionStatementStart() const;
    bool isTypedBindingStart() const;
    bool isClearSimpleTypeName(std::string_view name) const;
    bool shouldParseTypeFirstBinding() const;
    TypedBinding parseTypedBinding(std::string_view nameMessage, std::string_view typeMessage);
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
    Type parseTypeArgument(std::string_view message);
    std::unique_ptr<ast::Expression> parsePostfix();
    std::unique_ptr<ast::Expression> parsePrimary();
    std::unique_ptr<ast::Expression> parseParenthesizedExpression();
    std::unique_ptr<ast::Expression> parseIdentifierOrStructLiteral();
    std::unique_ptr<ast::Expression> parseFloatLiteral();
    std::unique_ptr<ast::Expression> parseBoolLiteral(bool value);
    std::unique_ptr<ast::Expression> parseArrayLiteral();
    std::unique_ptr<ast::Expression> parseStructLiteralAfterName(const Token& name,
                                                                 std::vector<Type> typeArgs);
    std::unique_ptr<ast::Expression> tryParseGenericStructLiteral(const Token& name);
    std::vector<ast::StructLiteralField> parseStructLiteralFields();
    std::vector<std::unique_ptr<ast::Expression>> parseCallArguments();
    ast::BinaryOperator binaryOperatorFromToken(TokenKind kind) const;

    std::span<const Token> tokens_;
    std::size_t index_ = 0;
    bool structLiteralAllowed_ = true;
    std::unordered_set<std::string> typeParamsInScope_;
    std::unordered_set<std::string> structNames_;
  };

} // namespace noria
