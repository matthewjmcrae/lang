#pragma once

#include "noria/Ast.hpp"
#include "noria/Types.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace noria {

  class LLVMGenerator {
  public:
    LLVMGenerator();
    ~LLVMGenerator();
    LLVMGenerator(const LLVMGenerator&) = delete;
    LLVMGenerator& operator=(const LLVMGenerator&) = delete;
    LLVMGenerator(LLVMGenerator&&) noexcept;
    LLVMGenerator& operator=(LLVMGenerator&&) noexcept;

    void setFunctionSpecializationTypeArgs(
        std::unordered_map<std::string, std::vector<Type>> typeArgsByFunction);
    void setStructSpecializationTypeArgs(
        std::unordered_map<std::string, std::vector<Type>> typeArgsByStruct);
    std::string generate(const ast::Module& module) const;

    enum class OwnershipMode {
      Borrow,
      Own,
    };

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
  };

} // namespace noria
