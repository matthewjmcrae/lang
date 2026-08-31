#include "noria/Diagnostic.hpp"

#include <cstdlib>
#include <iostream>

namespace {

  int failures = 0;

  void expectEq(const std::string& actual, const std::string& expected, const char* message) {
    if (actual != expected) {
      std::cerr << "FAIL: " << message << " (expected '" << expected << "', got '" << actual
                << "')\n";
      ++failures;
    }
  }

} // namespace

int main() {
  noria::SourceLocation noFile;
  noFile.line = 1;
  noFile.column = 2;
  expectEq(noria::formatDiagnostic(noFile, "msg"), "1:2: msg", "line/column without file");
  expectEq(noria::formatDiagnostic(noFile, noria::DiagnosticStage::Lexer, "msg"), "1:2: lexer: msg",
           "stage without file");

  noria::SourceLocation withFile;
  withFile.file = "std::mathx";
  withFile.line = 3;
  withFile.column = 4;
  expectEq(noria::formatDiagnostic(withFile, "err"), "std::mathx:3:4: err", "file without stage");
  expectEq(noria::formatDiagnostic(withFile, noria::DiagnosticStage::TypeCheck, "err"),
           "std::mathx:3:4: typecheck: err", "file with stage");

  if (failures != 0) {
    std::cerr << failures << " diagnostic location test failure(s)\n";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
