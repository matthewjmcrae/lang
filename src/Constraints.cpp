#include "noria/Constraints.hpp"

#include "noria/SemanticTables.hpp"

#include <algorithm>

namespace noria {

  std::vector<RequiredOperation> requiredOperations(ImplementationTag tag) {
    if (const ImplementationTagInfo* info = implementationTagInfo(tag)) {
      return info->requiredOperations;
    }
    return {};
  }

  bool supportsOperation(const Type& type, RequiredOperation operation) {
    const RequiredOperationInfo* info = requiredOperationInfo(operation);
    if (info == nullptr) {
      return false;
    }

    return std::find(info->supportedTypeKinds.begin(), info->supportedTypeKinds.end(), type.kind()) !=
           info->supportedTypeKinds.end();
  }

  std::string_view operationName(RequiredOperation operation) {
    if (const RequiredOperationInfo* info = requiredOperationInfo(operation)) {
      return info->name;
    }
    return "";
  }

} // namespace noria
