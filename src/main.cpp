#include "noria/AstPrinter.hpp"
#include "noria/Compiler.hpp"
#include "noria/Diagnostic.hpp"
#include "noria/Runtime.hpp"
#include "noria/Token.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <spawn.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

extern char** environ;

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

  const char* displayName(const char* argv0) {
    if (argv0 == nullptr || argv0[0] == '\0') {
      return "noria";
    }
    return argv0;
  }

  void printUsage(const char* argv0, std::ostream& out) {
    const char* name = displayName(argv0);
    out << "Noria compiler\n\n";
    out << "Usage:\n";
    out << "  " << name
        << " [--emit-tokens|--emit-ast] [--stdlib <dir>] [-O0|-O1|-O2|-O3] <input.noria> [-o "
           "output]\n";
    out << "  " << name
        << " build [--stdlib <dir>] [-O0|-O1|-O2|-O3] <input.noria> [-o executable]\n\n";
    out << "Examples:\n";
    out << "  " << name << " examples/basic/return_answer.noria\n";
    out << "  " << name << " examples/basic/return_answer.noria -o build/return_answer.ll\n\n";
    out << "  " << name << " --emit-tokens examples/basic/lexer_smoke.noria\n";
    out << "  " << name << " --emit-ast examples/basic/factorial.noria\n\n";
    out << "  " << name << " build examples/basic/factorial.noria -o build/factorial\n\n";
    out << "  " << name << " -O2 examples/basic/variables.noria -o build/variables.opt.ll\n\n";
    out << "Current output:\n";
    out << "  LLVM IR text by default, or debug output with --emit-tokens / --emit-ast\n";
  }

  void appendUnique(std::vector<std::filesystem::path>& paths, std::filesystem::path candidate) {
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(candidate, ec);
    if (!ec) {
      candidate = absolute.lexically_normal();
    }
    for (const auto& existing : paths) {
      if (existing == candidate) {
        return;
      }
    }
    paths.push_back(std::move(candidate));
  }

  std::optional<std::filesystem::path> platformExecutablePath() {
#if defined(__APPLE__)
    uint32_t size = 1024;
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
      if (size == 0) {
        return std::nullopt;
      }
      buffer.resize(size);
      if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return std::nullopt;
      }
    }
    return std::filesystem::path(buffer.data());
#elif defined(__linux__)
    std::error_code ec;
    std::filesystem::path path = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) {
      return std::nullopt;
    }
    if (path.is_absolute()) {
      return path;
    }
    return std::filesystem::path("/proc/self") / path;
#else
    return std::nullopt;
#endif
  }

  void collectArgv0Directories(std::vector<std::filesystem::path>& directories, const char* argv0) {
    if (argv0 == nullptr || argv0[0] == '\0') {
      return;
    }

    const std::filesystem::path given(argv0);
    if (given != given.filename()) {
      appendUnique(directories, given.parent_path());
      std::error_code ec;
      const auto canonical = std::filesystem::canonical(given, ec);
      if (!ec) {
        appendUnique(directories, canonical.parent_path());
      }
      return;
    }

    const char* pathEnv = std::getenv("PATH");
    if (pathEnv == nullptr) {
      return;
    }

    std::string_view remaining(pathEnv);
    while (true) {
      const auto colon = remaining.find(':');
      const std::string_view dirView =
          colon == std::string_view::npos ? remaining : remaining.substr(0, colon);
      const std::filesystem::path dir = dirView.empty()
                                            ? std::filesystem::path(".")
                                            : std::filesystem::path(std::string(dirView));
      const std::filesystem::path candidate = dir / given;
      std::error_code ec;
      if (std::filesystem::exists(candidate, ec) && !ec) {
        appendUnique(directories, dir);
        const auto canonical = std::filesystem::canonical(candidate, ec);
        if (!ec) {
          appendUnique(directories, canonical.parent_path());
        }
        return;
      }
      if (colon == std::string_view::npos) {
        return;
      }
      remaining.remove_prefix(colon + 1);
    }
  }

  std::vector<std::filesystem::path> compilerDirectories(const char* argv0) {
    std::vector<std::filesystem::path> directories;
    if (const auto platform = platformExecutablePath()) {
      appendUnique(directories, platform->parent_path());
      std::error_code ec;
      const auto canonical = std::filesystem::canonical(*platform, ec);
      if (!ec) {
        appendUnique(directories, canonical.parent_path());
      }
    }
    collectArgv0Directories(directories, argv0);
    return directories;
  }

  std::filesystem::path defaultStdlibRoot(const char* argv0) {
    if (const char* env = std::getenv("NORIA_STDLIB"); env != nullptr && env[0] != '\0') {
      return env;
    }

    const std::vector<std::filesystem::path> directories = compilerDirectories(argv0);
    if (directories.empty()) {
      throw noria::CompileError(
          "failed to locate the noria executable; pass --stdlib <dir> or set NORIA_STDLIB");
    }

    for (const auto& exeDir : directories) {
      const std::filesystem::path candidates[] = {
          exeDir / "stdlib",
          exeDir.parent_path() / "stdlib",
          exeDir.parent_path() / "share" / "noria" / "stdlib",
      };
      for (const auto& candidate : candidates) {
        std::error_code ec;
        if (!std::filesystem::is_directory(candidate, ec) || ec) {
          continue;
        }
        const auto canonical = std::filesystem::canonical(candidate, ec);
        if (!ec) {
          return canonical;
        }
        return candidate;
      }
    }

    return directories.front().parent_path() / "stdlib";
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
      printUsage(argv[0], std::cout);
      std::exit(0);
    }

    return false;
  }

  Options parseOptions(int argc, char** argv) {
    Options options;

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
      printUsage(displayName(argv[0]), std::cerr);
      throw noria::CompileError("missing input file");
    }

    if (options.stdlibRoot.empty()) {
      options.stdlibRoot = defaultStdlibRoot(argv[0]);
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

  std::string joinCommand(const std::vector<std::string>& args) {
    std::ostringstream out;
    for (std::size_t index = 0; index < args.size(); ++index) {
      if (index != 0) {
        out << ' ';
      }
      out << args[index];
    }
    return out.str();
  }

  void runCommand(const std::vector<std::string>& args) {
    if (args.empty()) {
      throw noria::CompileError("internal error: empty command");
    }

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) {
      argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    pid_t pid = 0;
    const int spawned =
        posix_spawnp(&pid, args.front().c_str(), nullptr, nullptr, argv.data(), environ);
    if (spawned != 0) {
      throw noria::CompileError("failed to launch " + args.front() + ": " + std::strerror(spawned));
    }

    int status = 0;
    pid_t waited = -1;
    do {
      waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
      throw noria::CompileError("failed to wait for " + args.front() + ": " + std::strerror(errno));
    }

    if (WIFEXITED(status)) {
      const int exitCode = WEXITSTATUS(status);
      if (exitCode != 0) {
        throw noria::CompileError("command failed (" + std::to_string(exitCode) +
                                  "): " + joinCommand(args));
      }
      return;
    }

    if (WIFSIGNALED(status)) {
      throw noria::CompileError("command terminated by signal " + std::to_string(WTERMSIG(status)) +
                                ": " + joinCommand(args));
    }

    throw noria::CompileError("command failed: " + joinCommand(args));
  }

  bool pathExists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec) && !ec;
  }

  std::string llvmToolPath(std::string_view toolName) {
    const char* llvmBinEnv = std::getenv("LLVM_BIN");
    if (llvmBinEnv != nullptr) {
      return std::string(llvmBinEnv) + "/" + std::string(toolName);
    }

    const std::filesystem::path currentHomebrewPath =
        std::filesystem::path("/opt/homebrew/opt/llvm/bin") / std::string(toolName);
    if (pathExists(currentHomebrewPath)) {
      return currentHomebrewPath.string();
    }

    const std::filesystem::path versionedHomebrewPath =
        std::filesystem::path("/opt/homebrew/opt/llvm@15/bin") / std::string(toolName);
    if (pathExists(versionedHomebrewPath)) {
      return versionedHomebrewPath.string();
    }

    return std::string(toolName);
  }

  // Homebrew LLVM's clang cannot link macOS host binaries (SDK / libSystem).
  // Native builds use the system driver; llvmToolPath stays for `opt` only.
  std::string hostClangPath() {
    const std::filesystem::path systemClang{"/usr/bin/clang"};
    if (pathExists(systemClang)) {
      return systemClang.string();
    }
    return "clang";
  }

  std::filesystem::path createUniqueTempDirectory() {
    std::error_code ec;
    const auto tmpRoot = std::filesystem::temp_directory_path(ec);
    if (ec) {
      throw noria::CompileError("failed to resolve temporary directory: " + ec.message());
    }

    std::string tmpl = (tmpRoot / "noria-XXXXXX").string();
    if (mkdtemp(tmpl.data()) == nullptr) {
      throw noria::CompileError(std::string("failed to create temporary directory: ") +
                                std::strerror(errno));
    }
    return tmpl;
  }

  struct ScopedTempDirectory {
    std::filesystem::path path;

    ScopedTempDirectory() : path(createUniqueTempDirectory()) {}
    ScopedTempDirectory(const ScopedTempDirectory&) = delete;
    ScopedTempDirectory& operator=(const ScopedTempDirectory&) = delete;

    ~ScopedTempDirectory() {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
    }
  };

  std::string optimizeLlvmIr(const std::string& llvmIr, int optimizationLevel) {
    if (optimizationLevel == 0) {
      return llvmIr;
    }

    const ScopedTempDirectory temp;
    const std::filesystem::path inputPath = temp.path / "input.ll";
    const std::filesystem::path outputPath = temp.path / "output.ll";

    writeOutput(inputPath, llvmIr);
    runCommand({llvmToolPath("opt"), "-O" + std::to_string(optimizationLevel), "-S",
                inputPath.string(), "-o", outputPath.string()});
    return readFile(outputPath);
  }

  void buildNativeExecutable(const std::filesystem::path& outputPath, const std::string& llvmIr) {
    const std::filesystem::path executable = outputPath.empty() ? "a.out" : outputPath;
    const ScopedTempDirectory temp;
    const std::filesystem::path llPath = temp.path / "program.ll";
    const std::string targetTriple = noria::runtime::targetTriple();

    writeOutput(llPath, llvmIr);

    std::vector<std::string> command{hostClangPath()};
    if (!targetTriple.empty()) {
      command.push_back("--target=" + targetTriple);
    }
    // Linux needs libm for llvm.sqrt.f64 / llvm.pow.f64; macOS provides them via libSystem.
    command.push_back(llPath.string());
    command.push_back("-lm");
    command.push_back("-o");
    command.push_back(executable.string());
    runCommand(command);
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
    const noria::PipelineOutput output = noria::compileSource(
        source, stopAfterForOutputMode(options.outputMode), compileOptionsFor(options));
    writePipelineOutput(options, output);
  }

} // namespace

int main(int argc, char** argv) {
  try {
    runCompiler(argc, argv);
  } catch (const noria::CompileError& error) {
    std::cerr << "noria: error: " << error.what() << "\n";
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "noria: error: " << error.what() << "\n";
    return 1;
  }

  return 0;
}
