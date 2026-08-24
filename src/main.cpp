#include "noria/AstPrinter.hpp"
#include "noria/Compiler.hpp"
#include "noria/Diagnostic.hpp"
#include "noria/Token.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

  enum class OutputMode {
    LlvmIr,
    Tokens,
    Ast,
    NativeExecutable,
  };

  struct Options {
    std::filesystem::path inputPath;
    std::filesystem::path outputPath;
    std::filesystem::path stdlibRoot;
    OutputMode outputMode = OutputMode::LlvmIr;
    int optimizationLevel = 0;
  };

  void printUsage(const char* argv0) {
    std::cerr << "Noria compiler\n\n";
    std::cerr << "Usage:\n";
    std::cerr << "  " << argv0
              << " [--emit-tokens|--emit-ast] [--stdlib <dir>] [-O0|-O1|-O2|-O3] <input.noria> [-o "
                 "output]\n";
    std::cerr << "  " << argv0
              << " build [--stdlib <dir>] [-O0|-O1|-O2|-O3] <input.noria> [-o executable]\n\n";
    std::cerr << "Examples:\n";
    std::cerr << "  " << argv0 << " examples/basic/return_answer.noria\n";
    std::cerr << "  " << argv0
              << " examples/basic/return_answer.noria -o build/return_answer.ll\n\n";
    std::cerr << "  " << argv0 << " --emit-tokens examples/basic/lexer_smoke.noria\n";
    std::cerr << "  " << argv0 << " --emit-ast examples/basic/factorial.noria\n\n";
    std::cerr << "  " << argv0 << " build examples/basic/factorial.noria -o build/factorial\n\n";
    std::cerr << "  " << argv0
              << " -O2 examples/basic/variables.noria -o build/variables.opt.ll\n\n";
    std::cerr << "Current output:\n";
    std::cerr << "  LLVM IR text by default, or debug output with --emit-tokens / --emit-ast\n";
  }

  std::filesystem::path defaultStdlibRoot(const char* argv0) {
    return std::filesystem::canonical(argv0).parent_path().parent_path() / "stdlib";
  }

  bool parseOptionToken(Options& options, int& index, int argc, char** argv) {
    const std::string arg = argv[index];
    if (arg == "build") {
      options.outputMode = OutputMode::NativeExecutable;
      return true;
    }

    if (arg == "--stdlib") {
      if (index + 1 >= argc) {
        throw noria::CompileError("expected directory after --stdlib");
      }
      options.stdlibRoot = argv[++index];
      return true;
    }

    if (arg == "-O0" || arg == "-O1" || arg == "-O2" || arg == "-O3") {
      options.optimizationLevel = arg[2] - '0';
      return true;
    }

    if (arg == "-o") {
      if (index + 1 >= argc) {
        throw noria::CompileError("expected path after -o");
      }
      options.outputPath = argv[++index];
      return true;
    }

    if (arg == "--emit-tokens") {
      options.outputMode = OutputMode::Tokens;
      return true;
    }

    if (arg == "--emit-ast") {
      options.outputMode = OutputMode::Ast;
      return true;
    }

    if (arg == "-h" || arg == "--help") {
      printUsage(argv[0]);
      std::exit(0);
    }

    return false;
  }

  Options parseOptions(int argc, char** argv) {
    Options options;
    options.stdlibRoot = defaultStdlibRoot(argv[0]);

    for (int index{1}; index < argc; ++index) {
      if (parseOptionToken(options, index, argc, argv)) {
        continue;
      }

      if (!options.inputPath.empty()) {
        throw noria::CompileError("only one input file is supported");
      }
      options.inputPath = argv[index];
    }

    if (options.inputPath.empty()) {
      throw noria::CompileError("missing input file");
    }

    return options;
  }

  std::string readFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
      throw noria::CompileError("failed to open input file: " + path.string());
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
  }

  void writeOutput(const std::filesystem::path& path, const std::string& text) {
    if (path.empty()) {
      std::cout << text;
      return;
    }

    std::ofstream file(path);
    if (!file) {
      throw noria::CompileError("failed to open output file: " + path.string());
    }
    file << text;
  }

  std::string shellQuote(const std::filesystem::path& path) {
    std::string quoted = "'";
    for (const char character : path.string()) {
      if (character == '\'') {
        quoted += "'\\''";
      } else {
        quoted += character;
      }
    }
    quoted += "'";
    return quoted;
  }

  void runCommand(const std::string& command) {
    const int status = std::system(command.c_str());
    if (status != 0) {
      throw noria::CompileError("command failed: " + command);
    }
  }

  std::string llvmToolPath(std::string_view toolName) {
    const char* llvmBinEnv = std::getenv("LLVM_BIN");
    if (llvmBinEnv != nullptr) {
      return std::string(llvmBinEnv) + "/" + std::string(toolName);
    }

    const std::filesystem::path currentHomebrewPath =
        std::filesystem::path("/opt/homebrew/opt/llvm/bin") / std::string(toolName);
    if (std::filesystem::exists(currentHomebrewPath)) {
      return currentHomebrewPath.string();
    }

    const std::filesystem::path versionedHomebrewPath =
        std::filesystem::path("/opt/homebrew/opt/llvm@15/bin") / std::string(toolName);
    if (std::filesystem::exists(versionedHomebrewPath)) {
      return versionedHomebrewPath.string();
    }

    return std::string(toolName);
  }

  std::filesystem::path temporaryLlvmPath(std::string_view suffix) {
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("noria-" + std::to_string(timestamp) + std::string(suffix));
  }

  std::string optimizeLlvmIr(const std::string& llvmIr, int optimizationLevel) {
    if (optimizationLevel == 0) {
      return llvmIr;
    }

    const std::filesystem::path inputPath = temporaryLlvmPath(".ll");
    const std::filesystem::path outputPath = temporaryLlvmPath(".opt.ll");

    writeOutput(inputPath, llvmIr);
    runCommand(shellQuote(llvmToolPath("opt")) + " -O" + std::to_string(optimizationLevel) +
               " -S " + shellQuote(inputPath) + " -o " + shellQuote(outputPath));

    const std::string optimizedIr = readFile(outputPath);
    std::filesystem::remove(inputPath);
    std::filesystem::remove(outputPath);
    return optimizedIr;
  }

  void buildNativeExecutable(const std::filesystem::path& outputPath, const std::string& llvmIr) {
    const std::filesystem::path executable = outputPath.empty() ? "a.out" : outputPath;
    const std::filesystem::path llPath = executable.string() + ".ll";

    writeOutput(llPath, llvmIr);
    runCommand("clang " + shellQuote(llPath) + " -o " + shellQuote(executable));
  }

  std::string escapeTokenText(const std::string& text) {
    std::string escaped;
    for (const char character : text) {
      switch (character) {
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      case '"':
        escaped += "\\\"";
        break;
      case '\\':
        escaped += "\\\\";
        break;
      default:
        escaped += character;
        break;
      }
    }
    return escaped;
  }

  std::string dumpTokens(const std::vector<noria::Token>& tokens) {
    std::ostringstream out;

    for (const auto& token : tokens) {
      out << tokenKindName(token.kind) << " \"" << escapeTokenText(token.text) << "\" "
          << token.location.line << ":" << token.location.column << "\n";
    }

    return out.str();
  }

  noria::StopAfter stopAfterForOutputMode(OutputMode outputMode) {
    switch (outputMode) {
    case OutputMode::Tokens:
      return noria::StopAfter::Tokens;
    case OutputMode::Ast:
      return noria::StopAfter::Ast;
    case OutputMode::LlvmIr:
    case OutputMode::NativeExecutable:
      return noria::StopAfter::Ir;
    }

    return noria::StopAfter::Ir;
  }

  noria::CompileOptions compileOptionsFor(const Options& options) {
    noria::CompileOptions compileOptions;
    compileOptions.stdlibRoot = options.stdlibRoot;
    compileOptions.rootFileName = options.inputPath.string();
    return compileOptions;
  }

  void writePipelineOutput(const Options& options, const noria::PipelineOutput& output) {
    if (options.outputMode == OutputMode::Tokens) {
      writeOutput(options.outputPath, dumpTokens(output.tokens));
      return;
    }

    if (options.outputMode == OutputMode::Ast) {
      std::ostringstream out;
      noria::printAst(output.module, out);
      writeOutput(options.outputPath, out.str());
      return;
    }

    const std::string llvmIr = optimizeLlvmIr(output.LLVM, options.optimizationLevel);
    if (options.outputMode == OutputMode::NativeExecutable) {
      buildNativeExecutable(options.outputPath, llvmIr);
      return;
    }

    writeOutput(options.outputPath, llvmIr);
  }

  void runCompiler(int argc, char** argv) {
    const Options options = parseOptions(argc, argv);
    const std::string source = readFile(options.inputPath);
    const noria::PipelineOutput output =
        noria::compileSource(source, stopAfterForOutputMode(options.outputMode),
                             compileOptionsFor(options));
    writePipelineOutput(options, output);
  }

} // namespace

int main(int argc, char** argv) {
  try {
    runCompiler(argc, argv);
  } catch (const noria::CompileError& error) {
    std::cerr << "noria: error: " << error.what() << "\n";
    return 1;
  }

  return 0;
}
