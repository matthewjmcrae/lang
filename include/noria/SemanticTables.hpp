#pragma once

#include "noria/Ast.hpp"
#include "noria/Constraints.hpp"
#include "noria/Types.hpp"

#include <cstddef>
#include <optional>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace noria {

  template <typename Enum>
  struct EnumHash {
    static_assert(std::is_enum_v<Enum>);

    std::size_t operator()(Enum value) const {
      return static_cast<std::size_t>(std::underlying_type_t<Enum>(value));
    }
  };

  enum class BinaryTypeCheckRule {
    Logical,
    Numeric,
    Integer,
    OrderedComparison,
    Equality,
  };

  enum class IntegerSafetyRule {
    None,
    SignedDivisionOrRemainder,
    ShiftCount,
  };

  enum class UnaryTypeCheckRule {
    Numeric,
    Boolean,
    Integer,
  };

  enum class UnaryCodegenRule {
    Negate,
    LogicalNot,
    BitNot,
  };

  struct BinaryOperatorInfo {
    std::string_view symbol;
    BinaryTypeCheckRule typeCheckRule;
    bool shortCircuit = false;
    bool comparison = false;
    std::string_view LLVMIntegerInstruction;
    std::string_view LLVMFloatInstruction;
    std::string_view LLVMIntegerPredicate;
    std::string_view LLVMFloatPredicate;
    IntegerSafetyRule integerSafetyRule = IntegerSafetyRule::None;
  };

  struct UnaryOperatorInfo {
    std::string_view symbol;
    UnaryTypeCheckRule typeCheckRule;
    UnaryCodegenRule codegenRule;
  };

  struct TypeKindInfo {
    std::string_view displayName;
    std::string_view LLVMName;
    std::string_view mangleAtom;
    std::optional<std::size_t> runtimeElementSize;
  };

  struct ImplementationTagInfo {
    std::string_view name;
    std::vector<RequiredOperation> requiredOperations;
  };

  struct RequiredOperationInfo {
    std::string_view name;
    std::vector<TypeKind> supportedTypeKinds;
  };

  enum class ContainerOperation {
    New,
    Get,
    Set,
    Contains,
    Insert,
    Drop,
    Clone,
  };

  struct StandardContainerInfo {
    StandardContainer kind;
    std::string_view structName;
    std::string_view modulePath;
    std::size_t typeArgumentCount;
  };

  inline const std::vector<StandardContainerInfo>& standardContainerTable() {
    static const std::vector<StandardContainerInfo> table = {
        {StandardContainer::Sequence, "sequence", "std::sequence", 2},
        {StandardContainer::Dictionary, "dictionary", "std::dictionary", 3},
        {StandardContainer::Set, "set", "std::set", 2},
    };
    return table;
  }

  inline const StandardContainerInfo* standardContainerInfo(std::string_view modulePath,
                                                             std::string_view structName) {
    for (const StandardContainerInfo& info : standardContainerTable()) {
      if (info.modulePath == modulePath && info.structName == structName) {
        return &info;
      }
    }
    return nullptr;
  }

  inline std::string_view containerOperationSourceName(StandardContainer container,
                                                       ContainerOperation operation) {
    switch (container) {
    case StandardContainer::Sequence:
      switch (operation) {
      case ContainerOperation::New: return "sequence_new";
      case ContainerOperation::Get: return "sequence_get";
      case ContainerOperation::Set: return "sequence_set";
      case ContainerOperation::Drop: return "sequence_drop";
      case ContainerOperation::Clone: return "sequence_clone";
      default: return "";
      }
    case StandardContainer::Dictionary:
      switch (operation) {
      case ContainerOperation::New: return "dictionary_new";
      case ContainerOperation::Get: return "dictionary_get";
      case ContainerOperation::Contains: return "dictionary_contains";
      case ContainerOperation::Insert: return "dictionary_insert";
      case ContainerOperation::Drop: return "dictionary_drop";
      case ContainerOperation::Clone: return "dictionary_clone";
      default: return "";
      }
    case StandardContainer::Set:
      switch (operation) {
      case ContainerOperation::New: return "set_new";
      case ContainerOperation::Contains: return "set_contains";
      case ContainerOperation::Drop: return "set_drop";
      case ContainerOperation::Clone: return "set_clone";
      default: return "";
      }
    }
    return "";
  }

  inline std::string_view containerOperationHiddenName(StandardContainer container,
                                                       ContainerOperation operation) {
    switch (container) {
    case StandardContainer::Sequence:
      switch (operation) {
      case ContainerOperation::New: return "__noria_sequence_new";
      case ContainerOperation::Get: return "__noria_sequence_get";
      case ContainerOperation::Set: return "__noria_sequence_set";
      case ContainerOperation::Drop: return "__noria_sequence_drop";
      case ContainerOperation::Clone: return "__noria_sequence_clone";
      default: return "";
      }
    case StandardContainer::Dictionary:
      switch (operation) {
      case ContainerOperation::New: return "__noria_dictionary_new";
      case ContainerOperation::Get: return "__noria_dictionary_get";
      case ContainerOperation::Contains: return "__noria_dictionary_contains";
      case ContainerOperation::Insert: return "__noria_dictionary_insert";
      case ContainerOperation::Drop: return "__noria_dictionary_drop";
      case ContainerOperation::Clone: return "__noria_dictionary_clone";
      default: return "";
      }
    case StandardContainer::Set:
      switch (operation) {
      case ContainerOperation::New: return "__noria_set_new";
      case ContainerOperation::Contains: return "__noria_set_contains";
      case ContainerOperation::Drop: return "__noria_set_drop";
      case ContainerOperation::Clone: return "__noria_set_clone";
      default: return "";
      }
    }
    return "";
  }

  inline const std::unordered_map<ast::BinaryOperator, BinaryOperatorInfo,
                                  EnumHash<ast::BinaryOperator>>&
  binaryOperatorTable() {
    static const std::unordered_map<ast::BinaryOperator, BinaryOperatorInfo,
                                    EnumHash<ast::BinaryOperator>>
        table = {
            {ast::BinaryOperator::Add,
             {"+", BinaryTypeCheckRule::Numeric, false, false, "add", "fadd", "", ""}},
            {ast::BinaryOperator::Subtract,
             {"-", BinaryTypeCheckRule::Numeric, false, false, "sub", "fsub", "", ""}},
            {ast::BinaryOperator::Multiply,
             {"*", BinaryTypeCheckRule::Numeric, false, false, "mul", "fmul", "", ""}},
            {ast::BinaryOperator::Divide,
             {"/", BinaryTypeCheckRule::Numeric, false, false, "sdiv", "fdiv", "", "",
              IntegerSafetyRule::SignedDivisionOrRemainder}},
            {ast::BinaryOperator::Modulo,
             {"%", BinaryTypeCheckRule::Integer, false, false, "srem", "", "", "",
              IntegerSafetyRule::SignedDivisionOrRemainder}},
            {ast::BinaryOperator::And,
             {"&&", BinaryTypeCheckRule::Logical, true, false, "", "", "", ""}},
            {ast::BinaryOperator::Or,
             {"||", BinaryTypeCheckRule::Logical, true, false, "", "", "", ""}},
            {ast::BinaryOperator::BitAnd,
             {"&", BinaryTypeCheckRule::Integer, false, false, "and", "", "", ""}},
            {ast::BinaryOperator::BitOr,
             {"|", BinaryTypeCheckRule::Integer, false, false, "or", "", "", ""}},
            {ast::BinaryOperator::BitXor,
             {"^", BinaryTypeCheckRule::Integer, false, false, "xor", "", "", ""}},
            {ast::BinaryOperator::Shl,
             {"<<", BinaryTypeCheckRule::Integer, false, false, "shl", "", "", "",
              IntegerSafetyRule::ShiftCount}},
            {ast::BinaryOperator::Shr,
             {">>", BinaryTypeCheckRule::Integer, false, false, "ashr", "", "", "",
              IntegerSafetyRule::ShiftCount}},
            {ast::BinaryOperator::Less,
             {"<", BinaryTypeCheckRule::OrderedComparison, false, true, "", "", "slt", "olt"}},
            {ast::BinaryOperator::LessEqual,
             {"<=", BinaryTypeCheckRule::OrderedComparison, false, true, "", "", "sle", "ole"}},
            {ast::BinaryOperator::Greater,
             {">", BinaryTypeCheckRule::OrderedComparison, false, true, "", "", "sgt", "ogt"}},
            {ast::BinaryOperator::GreaterEqual,
             {">=", BinaryTypeCheckRule::OrderedComparison, false, true, "", "", "sge", "oge"}},
            {ast::BinaryOperator::Equal,
             {"==", BinaryTypeCheckRule::Equality, false, true, "", "", "eq", "oeq"}},
            {ast::BinaryOperator::NotEqual,
             {"!=", BinaryTypeCheckRule::Equality, false, true, "", "", "ne", "une"}},
        };
    return table;
  }

  inline const BinaryOperatorInfo* binaryOperatorInfo(ast::BinaryOperator op) {
    const auto& table = binaryOperatorTable();
    const auto info = table.find(op);
    return info == table.end() ? nullptr : &info->second;
  }

  inline const std::unordered_map<std::string_view, ast::BinaryOperator>& binaryOperatorSymbolTable() {
    static const std::unordered_map<std::string_view, ast::BinaryOperator> table = {
        {"+", ast::BinaryOperator::Add},           {"-", ast::BinaryOperator::Subtract},
        {"*", ast::BinaryOperator::Multiply},      {"/", ast::BinaryOperator::Divide},
        {"%", ast::BinaryOperator::Modulo},        {"&&", ast::BinaryOperator::And},
        {"||", ast::BinaryOperator::Or},           {"&", ast::BinaryOperator::BitAnd},
        {"|", ast::BinaryOperator::BitOr},         {"^", ast::BinaryOperator::BitXor},
        {"<<", ast::BinaryOperator::Shl},          {">>", ast::BinaryOperator::Shr},
        {"<", ast::BinaryOperator::Less},          {"<=", ast::BinaryOperator::LessEqual},
        {">", ast::BinaryOperator::Greater},       {">=", ast::BinaryOperator::GreaterEqual},
        {"==", ast::BinaryOperator::Equal},        {"!=", ast::BinaryOperator::NotEqual},
    };
    return table;
  }

  inline std::optional<ast::BinaryOperator> binaryOperatorFromSymbol(std::string_view symbol) {
    const auto& table = binaryOperatorSymbolTable();
    const auto op = table.find(symbol);
    if (op != table.end()) {
      return op->second;
    }
    return std::nullopt;
  }

  inline const std::unordered_map<ast::UnaryOperator, UnaryOperatorInfo,
                                  EnumHash<ast::UnaryOperator>>&
  unaryOperatorTable() {
    static const std::unordered_map<ast::UnaryOperator, UnaryOperatorInfo,
                                    EnumHash<ast::UnaryOperator>>
        table = {
            {ast::UnaryOperator::Negate,
             {"-", UnaryTypeCheckRule::Numeric, UnaryCodegenRule::Negate}},
            {ast::UnaryOperator::Not,
             {"!", UnaryTypeCheckRule::Boolean, UnaryCodegenRule::LogicalNot}},
            {ast::UnaryOperator::BitNot,
             {"~", UnaryTypeCheckRule::Integer, UnaryCodegenRule::BitNot}},
        };
    return table;
  }

  inline const UnaryOperatorInfo* unaryOperatorInfo(ast::UnaryOperator op) {
    const auto& table = unaryOperatorTable();
    const auto info = table.find(op);
    return info == table.end() ? nullptr : &info->second;
  }

  inline const std::unordered_map<std::string_view, ast::UnaryOperator>& unaryOperatorSymbolTable() {
    static const std::unordered_map<std::string_view, ast::UnaryOperator> table = {
        {"-", ast::UnaryOperator::Negate},
        {"!", ast::UnaryOperator::Not},
        {"~", ast::UnaryOperator::BitNot},
    };
    return table;
  }

  inline std::optional<ast::UnaryOperator> unaryOperatorFromSymbol(std::string_view symbol) {
    const auto& table = unaryOperatorSymbolTable();
    const auto op = table.find(symbol);
    if (op != table.end()) {
      return op->second;
    }
    return std::nullopt;
  }

  inline const std::unordered_map<TypeKind, TypeKindInfo, EnumHash<TypeKind>>& typeKindTable() {
    static const std::unordered_map<TypeKind, TypeKindInfo, EnumHash<TypeKind>> table = {
        {TypeKind::I32, {"i32", "i32", "s.i32", 4}},
        {TypeKind::F64, {"f64", "double", "s.f64", 8}},
        {TypeKind::Bool, {"bool", "i1", "s.bool", 1}},
        {TypeKind::Str, {"str", "ptr", "s.str", 8}},
        {TypeKind::Array, {"", "ptr", "", 8}},
        {TypeKind::Struct, {"", "", "", std::nullopt}},
        {TypeKind::TypeParam, {"", "", "", std::nullopt}},
        {TypeKind::ImplTag, {"", "", "", std::nullopt}},
        {TypeKind::RawPtr, {"__rt_ptr", "ptr", "s.rt_ptr", 8}},
        {TypeKind::Void, {"void", "void", "s.void", std::nullopt}},
    };
    return table;
  }

  inline const TypeKindInfo* typeKindInfo(TypeKind kind) {
    const auto& table = typeKindTable();
    const auto info = table.find(kind);
    return info == table.end() ? nullptr : &info->second;
  }

  inline const std::unordered_map<ImplementationTag, ImplementationTagInfo,
                                  EnumHash<ImplementationTag>>&
  implementationTagTable() {
    static const std::unordered_map<ImplementationTag, ImplementationTagInfo,
                                    EnumHash<ImplementationTag>>
        table = {
            {ImplementationTag::Arr, {"arr", {}}},
            {ImplementationTag::List, {"list", {}}},
            {ImplementationTag::Bst,
             {"bst", {RequiredOperation::LessThan, RequiredOperation::Equality}}},
            {ImplementationTag::Hashmap,
             {"hashmap", {RequiredOperation::Equality, RequiredOperation::Hash}}},
        };
    return table;
  }

  inline const ImplementationTagInfo* implementationTagInfo(ImplementationTag tag) {
    const auto& table = implementationTagTable();
    const auto info = table.find(tag);
    return info == table.end() ? nullptr : &info->second;
  }

  inline const std::unordered_map<std::string_view, ImplementationTag>& implementationTagNameTable() {
    static const std::unordered_map<std::string_view, ImplementationTag> table = {
        {"arr", ImplementationTag::Arr},
        {"list", ImplementationTag::List},
        {"bst", ImplementationTag::Bst},
        {"hashmap", ImplementationTag::Hashmap},
        {"hashset", ImplementationTag::Hashmap}
    };
    return table;
  }

  inline std::optional<StandardContainer>
  standardContainerKindFromStructName(std::string_view name) {
    const std::size_t dollar = name.find('$');
    const std::string_view base = dollar == std::string_view::npos ? name : name.substr(0, dollar);
    if (base == "sequence" || base == "Sequence") {
      return StandardContainer::Sequence;
    }
    if (base == "dictionary" || base == "Dictionary") {
      return StandardContainer::Dictionary;
    }
    if (base == "set" || base == "Set") {
      return StandardContainer::Set;
    }
    return std::nullopt;
  }

  inline const std::unordered_map<RequiredOperation, RequiredOperationInfo,
                                  EnumHash<RequiredOperation>>&
  requiredOperationTable() {
    static const std::unordered_map<RequiredOperation, RequiredOperationInfo,
                                    EnumHash<RequiredOperation>>
        table = {
            {RequiredOperation::LessThan, {"<", {TypeKind::I32, TypeKind::F64}}},
            {RequiredOperation::Equality,
             {"==", {TypeKind::I32, TypeKind::F64, TypeKind::Bool, TypeKind::Str}}},
            {RequiredOperation::Hash, {"hash", {TypeKind::I32, TypeKind::Bool, TypeKind::Str}}},
        };
    return table;
  }

  inline const RequiredOperationInfo* requiredOperationInfo(RequiredOperation operation) {
    const auto& table = requiredOperationTable();
    const auto info = table.find(operation);
    return info == table.end() ? nullptr : &info->second;
  }

} // namespace noria
