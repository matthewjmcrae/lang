#include "noria/IrEmitter.hpp"

#include <string>

namespace noria {

  IrEmitter::IrEmitter(std::ostringstream& out, int nextTemporary, int nextLabel)
      : out_(out), nextTemporary_(nextTemporary), nextLabel_(nextLabel) {}

  std::string IrEmitter::freshTemp() {
    return "%t" + std::to_string(nextTemporary_++);
  }

  int IrEmitter::freshTempCounter() {
    return nextTemporary_++;
  }

  int IrEmitter::freshLabelId() {
    return nextLabel_++;
  }

  void IrEmitter::emitLoad(const Type& type, const std::string& slot, const std::string& result) {
    out_ << "  " << result << " = load " << llvmType(type) << ", ptr " << slot << "\n";
  }

  void IrEmitter::emitStore(const Type& type, const std::string& value, const std::string& slot) {
    out_ << "  store " << llvmType(type) << " " << value << ", ptr " << slot << "\n";
  }

  void IrEmitter::emitAlloca(const Type& type, const std::string& slot) {
    out_ << "  " << slot << " = alloca " << llvmType(type) << "\n";
  }

  void IrEmitter::emitBranch(const std::string& label) {
    out_ << "  br label %" << label << "\n";
  }

  void IrEmitter::emitCondBranch(const std::string& condition, const std::string& thenLabel,
                                 const std::string& elseLabel) {
    out_ << "  br i1 " << condition << ", label %" << thenLabel << ", label %" << elseLabel << "\n";
  }

  void IrEmitter::emitLabel(const std::string& name) {
    out_ << name << ":\n";
  }

  void IrEmitter::line(const std::string& text) {
    out_ << "  " << text << "\n";
  }

} // namespace noria
