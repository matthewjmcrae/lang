#pragma once

#include "noria/Token.hpp"

#include <stdexcept>
#include <string>
#include <string_view>

namespace noria {

  enum class DiagnosticStage { Lexer, TypeCheck };

  std::string formatDiagnostic(SourceLocation location, std::string_view message);
  std::string formatDiagnostic(SourceLocation location, DiagnosticStage stage,
                               std::string_view message);

  class CompileError : public std::runtime_error {
  public:
    explicit CompileError(const std::string& message) : std::runtime_error(message) {}
  };

} // namespace noria
