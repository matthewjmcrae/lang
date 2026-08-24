#include "noria/Constraints.hpp"

namespace noria {

  std::vector<RequiredOperation> requiredOperations(ImplementationTag tag) {
    switch (tag) {
    case ImplementationTag::Arr:
    case ImplementationTag::List:
      return {};
    case ImplementationTag::Bst:
      return {RequiredOperation::LessThan, RequiredOperation::Equality};
    case ImplementationTag::Hashmap:
      return {RequiredOperation::Equality, RequiredOperation::Hash};
    }
    return {};
  }

  bool supportsOperation(const Type& type, RequiredOperation operation) {
    switch (operation) {
    case RequiredOperation::LessThan:
      return type.kind == TypeKind::I32 || type.kind == TypeKind::F64;
    case RequiredOperation::Equality:
      return type.kind == TypeKind::I32 || type.kind == TypeKind::F64 ||
             type.kind == TypeKind::Bool || type.kind == TypeKind::Str;
    case RequiredOperation::Hash:
      return type.kind == TypeKind::I32 || type.kind == TypeKind::Bool ||
             type.kind == TypeKind::Str;
    }
    return false;
  }

  std::string_view operationName(RequiredOperation operation) {
    switch (operation) {
    case RequiredOperation::LessThan:
      return "<";
    case RequiredOperation::Equality:
      return "==";
    case RequiredOperation::Hash:
      return "hash";
    }
    return "";
  }

} // namespace noria
