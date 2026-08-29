#include "noria/Codegen.hpp"

#include "CodegenInternal.hpp"

#include <memory>
#include <utility>

namespace noria {

  LLVMGenerator::LLVMGenerator() : impl_(std::make_unique<Impl>()) {}
  LLVMGenerator::~LLVMGenerator() = default;
  LLVMGenerator::LLVMGenerator(LLVMGenerator&&) noexcept = default;
  LLVMGenerator& LLVMGenerator::operator=(LLVMGenerator&&) noexcept = default;

  void LLVMGenerator::setFunctionSpecializationTypeArgs(
      std::unordered_map<std::string, std::vector<Type>> typeArgsByFunction) {
    impl_->functionSpecializationTypeArgs = std::move(typeArgsByFunction);
  }

  void LLVMGenerator::setStructSpecializationTypeArgs(
      std::unordered_map<std::string, std::vector<Type>> typeArgsByStruct) {
    impl_->structSpecializationTypeArgs = std::move(typeArgsByStruct);
  }

  std::string LLVMGenerator::generate(const ast::Module& module) const {
    return impl_->module.generateModule(module, impl_->functionSpecializationTypeArgs,
                                        impl_->structSpecializationTypeArgs);
  }

} // namespace noria
