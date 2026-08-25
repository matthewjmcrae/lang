#pragma once

#include "noria/Codegen.hpp"

#include <memory>

namespace noria {

  enum class CodegenStrategyKind { Module, Builtins, Expressions, Places, Statements, Structs };

  class CodegenStrategy {
  public:
    virtual ~CodegenStrategy() = default;
    virtual CodegenStrategyKind kind() const noexcept = 0;
    virtual std::string generate(const LLVMGenerator&, const ast::Module&) const;
  };

  class CodegenModule final : public CodegenStrategy {
  public:
    CodegenStrategyKind kind() const noexcept override { return CodegenStrategyKind::Module; }
    std::string generate(const LLVMGenerator&, const ast::Module&) const override;
  };
  class CodegenBuiltins final : public CodegenStrategy { public: CodegenStrategyKind kind() const noexcept override { return CodegenStrategyKind::Builtins; } };
  class CodegenExpressions final : public CodegenStrategy { public: CodegenStrategyKind kind() const noexcept override { return CodegenStrategyKind::Expressions; } };
  class CodegenPlaces final : public CodegenStrategy { public: CodegenStrategyKind kind() const noexcept override { return CodegenStrategyKind::Places; } };
  class CodegenStatements final : public CodegenStrategy { public: CodegenStrategyKind kind() const noexcept override { return CodegenStrategyKind::Statements; } };
  class CodegenStructs final : public CodegenStrategy { public: CodegenStrategyKind kind() const noexcept override { return CodegenStrategyKind::Structs; } };

  std::unique_ptr<CodegenStrategy> makeCodegenStrategy(CodegenStrategyKind kind);
  LLVMGenerator makeLLVMGeneratorWithModuleStrategy();

} // namespace noria
