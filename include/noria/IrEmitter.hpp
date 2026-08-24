#pragma once

#include "noria/Types.hpp"

#include <sstream>
#include <string>

namespace noria {

  class IREmitter {
  public:
    IREmitter(std::ostringstream& out, int nextTemporary = 0, int nextLabel = 0);

    std::string freshTemp();
    int freshTempCounter();
    int freshLabelId();

    void emitLoad(const Type& type, const std::string& slot, const std::string& result);
    void emitStore(const Type& type, const std::string& value, const std::string& slot);
    void emitAlloca(const Type& type, const std::string& slot);
    void emitBranch(const std::string& label);
    void emitCondBranch(const std::string& condition, const std::string& thenLabel,
                        const std::string& elseLabel);
    void emitLabel(const std::string& name);
    void line(const std::string& text);

  private:
    std::ostringstream& out_;
    int nextTemporary_;
    int nextLabel_;
  };

} // namespace noria
