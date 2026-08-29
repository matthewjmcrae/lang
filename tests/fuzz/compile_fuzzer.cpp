#include "noria/Compiler.hpp"
#include "noria/CompilerCache.hpp"
#include "noria/Diagnostic.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // Bound inputs already via -max_len; still reject empty for stability.
  if (data == nullptr || size == 0) {
    return 0;
  }

  const std::string_view source(reinterpret_cast<const char*>(data), size);
  noria::processCompilerCache().clear();

  try {
    (void)noria::compileSource(source, noria::StopAfter::Ir);
  } catch (const noria::CompileError&) {
    // Expected for invalid inputs.
  } catch (const std::exception&) {
    // Unexpected C++ exception: treat as a fuzzer finding.
    __builtin_trap();
  } catch (...) {
    __builtin_trap();
  }

  return 0;
}
