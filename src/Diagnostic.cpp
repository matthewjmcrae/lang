#include "noria/Diagnostic.hpp"

namespace noria {

  namespace {

    const char* stageLabel(DiagnosticStage stage) {
      switch (stage) {
      case DiagnosticStage::Lexer:
        return "lexer";
      case DiagnosticStage::TypeCheck:
        return "typecheck";
      case DiagnosticStage::Import:
        return "import";
      }
      return "";
    }

    std::string formatLocationPrefix(SourceLocation location) {
      if (location.file.empty()) {
        return std::to_string(location.line) + ":" + std::to_string(location.column) + ": ";
      }
      return location.file + ":" + std::to_string(location.line) + ":" +
             std::to_string(location.column) + ": ";
    }

  } // namespace

  std::string formatDiagnostic(SourceLocation location, std::string_view message) {
    return formatLocationPrefix(location) + std::string(message);
  }

  std::string formatDiagnostic(SourceLocation location, DiagnosticStage stage,
                               std::string_view message) {
    return formatLocationPrefix(location) + stageLabel(stage) + ": " + std::string(message);
  }

} // namespace noria
