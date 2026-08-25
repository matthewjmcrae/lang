#include "noria/Codegen.hpp"

#include "codegen/CodegenInternal.hpp"

#include <memory>
#include <utility>

namespace noria {

  LLVMGenerator::LLVMGenerator() : impl_(std::make_unique<Impl>()) {}
  LLVMGenerator::~LLVMGenerator() = default;

  LLVMGenerator::LLVMGenerator(const LLVMGenerator& other)
      : impl_(std::make_unique<Impl>(*other.impl_)) {}

  LLVMGenerator& LLVMGenerator::operator=(const LLVMGenerator& other) {
    if (this != &other) {
      impl_ = std::make_unique<Impl>(*other.impl_);
    }
    return *this;
  }

  LLVMGenerator::LLVMGenerator(LLVMGenerator&& other) noexcept = default;
  LLVMGenerator& LLVMGenerator::operator=(LLVMGenerator&& other) noexcept = default;

  void LLVMGenerator::setFunctionSpecializationTypeArgs(
      std::unordered_map<std::string, std::vector<Type>> typeArgsByFunction) {
    impl_->setFunctionSpecializationTypeArgs(std::move(typeArgsByFunction));
  }

  std::string LLVMGenerator::generate(const ast::Module& module) const {
    return impl_->generate(module);
  }

} // namespace noria
