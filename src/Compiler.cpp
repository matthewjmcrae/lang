#include "noria/Compiler.hpp"

#include "noria/Codegen.hpp"
#include "noria/Lexer.hpp"
#include "noria/Parser.hpp"
#include "noria/TypeChecker.hpp"

namespace noria {

  CompileOutput compileSource(std::string_view source, StopAfter stopAfter) {
    CompileOutput output;

    Lexer lexer(source);
    output.tokens = lexer.lex();
    if (stopAfter == StopAfter::Tokens) {
      return output;
    }

    Parser parser(output.tokens);
    output.module = parser.parseModule();
    if (stopAfter == StopAfter::Ast) {
      return output;
    }

    TypeChecker checker;
    checker.check(output.module);
    if (stopAfter == StopAfter::Typed) {
      return output;
    }

    LlvmIrTextGenerator generator;
    output.llvmIr = generator.generate(output.module);
    return output;
  }

} // namespace noria
