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
    LLVMGenerator(const LLVMGenerator& other);
    LLVMGenerator& operator=(const LLVMGenerator& other);
    LLVMGenerator(LLVMGenerator&& other) noexcept;
    LLVMGenerator& operator=(LLVMGenerator&& other) noexcept;

    void setFunctionSpecializationTypeArgs(
        std::unordered_map<std::string, std::vector<Type>> typeArgsByFunction);
    std::string generate(const ast::Module& module) const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
  };

} // namespace noria
