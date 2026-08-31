#include "noria/IrEmitter.hpp"

#include <string>

namespace noria {

  IREmitter::IREmitter(std::ostringstream& out, int nextTemporary, int nextLabel)
      : out_(out), nextTemporary_(nextTemporary), nextLabel_(nextLabel) {}

  std::string IREmitter::freshTemp() {
    return "%t" + std::to_string(nextTemporary_++);
  }

  int IREmitter::freshTempCounter() {
    return nextTemporary_++;
  }

  int IREmitter::freshLabelId() {
    return nextLabel_++;
  }

  void IREmitter::emitLoad(const Type& type, const std::string& slot, const std::string& result) {
    out_ << "  " << result << " = load " << LLVMType(type) << ", ptr " << slot << "\n";
  }

  void IREmitter::emitStore(const Type& type, const std::string& value, const std::string& slot) {
    out_ << "  store " << LLVMType(type) << " " << value << ", ptr " << slot << "\n";
  }

  void IREmitter::emitAlloca(const Type& type, const std::string& slot) {
    out_ << "  " << slot << " = alloca " << LLVMType(type) << "\n";
  }

  void IREmitter::emitBranch(const std::string& label) {
    out_ << "  br label %" << label << "\n";
  }

  void IREmitter::emitCondBranch(const std::string& condition, const std::string& thenLabel,
                                 const std::string& elseLabel) {
    out_ << "  br i1 " << condition << ", label %" << thenLabel << ", label %" << elseLabel << "\n";
  }

  void IREmitter::emitLabel(const std::string& name) {
    out_ << name << ":\n";
  }

  void IREmitter::line(const std::string& text) {
    out_ << "  " << text << "\n";
  }

} // namespace noria
