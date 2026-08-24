#pragma once

#include "noria/Ast.hpp"
#include "noria/Token.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noria {

  enum class StopAfter { Tokens, Ast, Typed, Ir };

  struct CompileOptions {
    std::optional<std::filesystem::path> stdlibRoot;
    std::string rootFileName;
  };

  struct PipelineOutput {
    std::vector<Token> tokens;
    ast::Module module;
    std::string LLVM;

    PipelineOutput() = default;
    PipelineOutput(PipelineOutput&&) = default;
    PipelineOutput& operator=(PipelineOutput&&) = default;
    PipelineOutput(const PipelineOutput&) = delete;
    PipelineOutput& operator=(const PipelineOutput&) = delete;
  };

  PipelineOutput compileSource(std::string_view source, StopAfter stopAfter,
                               const CompileOptions& options = {});

} // namespace noria
