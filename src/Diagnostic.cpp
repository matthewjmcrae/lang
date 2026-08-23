#include "noria/Diagnostic.hpp"

namespace noria {

  namespace {

    const char* stageLabel(DiagnosticStage stage) {
      switch (stage) {
      case DiagnosticStage::Lexer:
        return "lexer";
      case DiagnosticStage::TypeCheck:
        return "typecheck";
      }
      return "";
    }

  } // namespace

  std::string formatDiagnostic(SourceLocation location, std::string_view message) {
    return std::to_string(location.line) + ":" + std::to_string(location.column) + ": " +
           std::string(message);
  }

  std::string formatDiagnostic(SourceLocation location, DiagnosticStage stage,
                               std::string_view message) {
    return std::to_string(location.line) + ":" + std::to_string(location.column) + ": " +
           stageLabel(stage) + ": " + std::string(message);
  }

} // namespace noria
