#pragma once

#include "noria/Ast.hpp"
#include "noria/Token.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace noria {

  enum class StopAfter { Tokens, Ast, Typed, Ir };

  struct CompileOutput {
    std::vector<Token> tokens;
    ast::Module module;
    std::string llvmIr;

    CompileOutput() = default;
    CompileOutput(CompileOutput&&) = default;
    CompileOutput& operator=(CompileOutput&&) = default;
    CompileOutput(const CompileOutput&) = delete;
    CompileOutput& operator=(const CompileOutput&) = delete;
  };

  CompileOutput compileSource(std::string_view source, StopAfter stopAfter);

} // namespace noria
