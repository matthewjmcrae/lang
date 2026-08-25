#include "noria/Codegen.hpp"

#include "CodegenStrategy.hpp"

#include <memory>
#include <concepts>
#include <stdexcept>
#include <utility>

namespace noria {

  static_assert(std::derived_from<CodegenModule, CodegenStrategy>);
  static_assert(std::derived_from<CodegenBuiltins, CodegenStrategy>);
  static_assert(std::derived_from<CodegenExpressions, CodegenStrategy>);
  static_assert(std::derived_from<CodegenPlaces, CodegenStrategy>);
  static_assert(std::derived_from<CodegenStatements, CodegenStrategy>);
  static_assert(std::derived_from<CodegenStructs, CodegenStrategy>);

  std::string CodegenStrategy::generate(const LLVMGenerator&, const ast::Module&) const {
    throw std::logic_error("codegen: invalid strategy endpoint");
  }

  std::string CodegenModule::generate(const LLVMGenerator& generator,
                                      const ast::Module& module) const {
    return generator.generateModule(module);
  }

  std::unique_ptr<CodegenStrategy> makeCodegenStrategy(CodegenStrategyKind kind) {
    switch (kind) {
    case CodegenStrategyKind::Module: return std::make_unique<CodegenModule>();
    case CodegenStrategyKind::Builtins: return std::make_unique<CodegenBuiltins>();
    case CodegenStrategyKind::Expressions: return std::make_unique<CodegenExpressions>();
    case CodegenStrategyKind::Places: return std::make_unique<CodegenPlaces>();
    case CodegenStrategyKind::Statements: return std::make_unique<CodegenStatements>();
    case CodegenStrategyKind::Structs: return std::make_unique<CodegenStructs>();
    }
    throw std::logic_error("codegen: unknown strategy");
  }

  LLVMGenerator makeLLVMGeneratorWithModuleStrategy() { return LLVMGenerator{}; }

  LLVMGenerator::LLVMGenerator()
      : activeStrategy_(makeCodegenStrategy(CodegenStrategyKind::Module)) {}
  LLVMGenerator::~LLVMGenerator() = default;
  LLVMGenerator::LLVMGenerator(LLVMGenerator&& other) noexcept = default;
  LLVMGenerator& LLVMGenerator::operator=(LLVMGenerator&& other) noexcept = default;

  LLVMGenerator::StrategyScope::StrategyScope(const LLVMGenerator& generator,
                                               CodegenStrategyKind requested)
      : generator_(generator) {
    if (generator_.activeStrategy_->kind() != requested) {
      previous_ = std::move(generator_.activeStrategy_);
      generator_.activeStrategy_ = makeCodegenStrategy(requested);
    }
  }

  LLVMGenerator::StrategyScope::~StrategyScope() {
    if (previous_) {
      generator_.activeStrategy_ = std::move(previous_);
    }
  }

  LLVMGenerator::StrategyScope LLVMGenerator::activate(CodegenStrategyKind requested) const {
    return StrategyScope(*this, requested);
  }

  void LLVMGenerator::setFunctionSpecializationTypeArgs(
      std::unordered_map<std::string, std::vector<Type>> typeArgsByFunction) {
    functionSpecializationTypeArgs_ = std::move(typeArgsByFunction);
  }

  std::string LLVMGenerator::generate(const ast::Module& module) const {
    return activeStrategy_->generate(*this, module);
  }

} // namespace noria
