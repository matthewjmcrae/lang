#include "CodegenInternal.hpp"

#include "noria/Builtins.hpp"
#include "noria/Diagnostic.hpp"
#include "noria/Monomorphize.hpp"
#include "noria/Runtime.hpp"
#include "noria/SemanticTables.hpp"

#include "CodegenSupport.hpp"
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace noria::codegen_detail {

  ExpressionEmitter::ExpressionVisitor::ExpressionVisitor(const ExpressionEmitter& state,
                                                          IREmitter& emitter,
                                                          FunctionCodegenContext& context,
                                                          std::optional<Type> expectedType,
                                                          LLVMGenerator::OwnershipMode ownership)
      : ExpressionOnlyVisitor("codegen"), state_(state), emitter_(emitter), context_(context),
        expectedType_(std::move(expectedType)), ownership_(ownership) {}

  void ExpressionEmitter::ExpressionVisitor::visit(const ast::IntegerLiteral& integer) {
    result_ = Value{std::to_string(integer.value), Type::i32()};
  }

  void ExpressionEmitter::ExpressionVisitor::visit(const ast::FloatLiteral& floating) {
    result_ = Value{formatLLVMFloatLiteral(floating.value), Type::f64()};
  }

  void ExpressionEmitter::ExpressionVisitor::visit(const ast::StringLiteral& stringLiteral) {
    result_ = state_.generateStringLiteral(stringLiteral, emitter_, context_);
  }

  void ExpressionEmitter::ExpressionVisitor::visit(const ast::BoolLiteral& boolean) {
    result_ = Value{boolean.value ? "true" : "false", Type::boolean()};
  }

  void ExpressionEmitter::ExpressionVisitor::visit(const ast::UnaryExpression& unary) {
    const Value operand = state_.generateRvalue(*unary.operand, emitter_, context_);
    const std::string result = emitter_.freshTemp();
    const UnaryOperatorInfo* info = unaryOperatorInfo(unary.op);
    if (info == nullptr) {
      throw CompileError("codegen: internal error: unknown unary operator");
    }

    if (info->codegenRule == UnaryCodegenRule::Negate) {
      if (operand.type == Type::f64()) {
        emitter_.line(result + " = fneg double " + operand.text);
        result_ = Value{result, Type::f64()};
        return;
      }
      emitter_.line(result + " = sub i32 0, " + operand.text);
      result_ = Value{result, Type::i32()};
      return;
    }

    if (info->codegenRule == UnaryCodegenRule::LogicalNot) {
      emitter_.line(result + " = xor i1 " + operand.text + ", true");
      result_ = Value{result, Type::boolean()};
      return;
    }

    if (info->codegenRule == UnaryCodegenRule::BitNot) {
      emitter_.line(result + " = xor i32 " + operand.text + ", -1");
      result_ = Value{result, Type::i32()};
      return;
    }

    throw CompileError("codegen: internal error: unknown unary codegen rule");
  }

  void ExpressionEmitter::ExpressionVisitor::visit(const ast::CastExpression& castExpression) {
    result_ = state_.generateCastExpression(castExpression, emitter_, context_);
  }

  void ExpressionEmitter::ExpressionVisitor::visit(const ast::BinaryExpression& binary) {
    result_ = state_.generateBinaryExpression(binary, emitter_, context_);
  }

  void ExpressionEmitter::ExpressionVisitor::visit(const ast::IdentifierExpression& identifier) {
    const LocalBinding& local = context_.lookupLocal(identifier.name);

    const std::string result = emitter_.freshTemp();
    emitter_.emitLoad(local.type, local.slot, result);
    Value loaded{result, local.type, false};
    if (ownership_ == LLVMGenerator::OwnershipMode::Own &&
        state_.ownership_.typeNeedsDrop(local.type, context_)) {
      result_ = state_.ownership_.emitCloneValue(loaded, emitter_, context_);
      return;
    }
    result_ = loaded;
  }

  void ExpressionEmitter::ExpressionVisitor::visit(const ast::CallExpression& call) {
    if (auto builtin = state_.builtins_.tryGenerateBuiltinCall(call, emitter_, context_, state_)) {
      result_ = *builtin;
      return;
    }

    const auto function = context_.module.functions.find(call.callee);
    if (function == context_.module.functions.end())
      throw CompileError("codegen: unknown function '" + call.callee + "'");

    std::vector<Value> arguments;
    arguments.reserve(call.arguments.size());

    for (std::size_t index{}; index < call.arguments.size(); ++index) {
      const Type expectedType = function->second.parameterTypes[index];
      arguments.push_back(state_.generateRvalue(*call.arguments[index], emitter_, context_,
                                                expectedType,
                                                LLVMGenerator::OwnershipMode::Borrow));
    }

    const bool returnsVoid = function->second.returnType == Type::voidType();
    const std::string result = returnsVoid ? "" : emitter_.freshTemp();
    std::string callLine = returnsVoid
                               ? "call void @" + call.callee + "("
                               : result + " = call " + LLVMType(function->second.returnType) +
                                     " @" + call.callee + "(";
    for (std::size_t index{}; index < arguments.size(); ++index) {
      if (index != 0)
        callLine += ", ";

      callLine += LLVMType(arguments[index].type) + " " + arguments[index].text;
    }
    callLine += ")";
    emitter_.line(callLine);

    for (const Value& argument : arguments) {
      state_.ownership_.emitReleaseIfOwned(argument, emitter_, context_);
    }

    if (returnsVoid) {
      result_ = Value{"", Type::voidType()};
      return;
    }

    Value returnValue{result, function->second.returnType, false};
    if (state_.ownership_.typeNeedsDrop(function->second.returnType, context_)) {
      returnValue.owned = true;
    }
    result_ = returnValue;
  }

  void ExpressionEmitter::ExpressionVisitor::visit(const ast::ArrayLiteral& literal) {
    result_ = state_.generateArrayLiteral(literal, emitter_, context_, expectedType_);
  }

  void ExpressionEmitter::ExpressionVisitor::visit(const ast::IndexExpression& index) {
    result_ = state_.generateIndexExpression(index, emitter_, context_, ownership_);
  }

  void ExpressionEmitter::ExpressionVisitor::visit(const ast::StructLiteral& literal) {
    result_ = state_.structs_.generateStructLiteral(state_, state_.ownership_, literal, emitter_,
                                                    context_);
  }

  void ExpressionEmitter::ExpressionVisitor::visit(const ast::FieldAccessExpression& access) {
    Value field = state_.structs_.generateFieldAccess(state_, access, emitter_, context_);
    if (ownership_ == LLVMGenerator::OwnershipMode::Own &&
        state_.ownership_.typeNeedsDrop(field.type, context_)) {
      field = state_.ownership_.emitCloneValue(field, emitter_, context_);
    }
    result_ = field;
  }

  Value ExpressionEmitter::generateStringLiteral(const ast::StringLiteral& literal,
                                                 IREmitter& emitter,
                                                 FunctionCodegenContext& context) const {
    return Value{memory_.emitCStringPointer(literal.value, emitter, context), Type::str(), false};
  }

  Value ExpressionEmitter::generateArrayLiteral(const ast::ArrayLiteral& literal,
                                                IREmitter& emitter, FunctionCodegenContext& context,
                                                const std::optional<Type>& expectedType) const {
    std::optional<Type> expectedElementType;
    if (expectedType && expectedType->kind() == TypeKind::Array) {
      expectedElementType = expectedType->elementType();
    }

    std::vector<Value> elements;
    elements.reserve(literal.elements.size());

    for (const auto& element : literal.elements) {
      elements.push_back(generateRvalue(*element, emitter, context, expectedElementType));
    }

    if (elements.empty()) {
      if (!expectedElementType) {
        throw CompileError("codegen: empty array literal missing expected array type");
      }
      return module().emitDefaultValue(Type::array(*expectedElementType), emitter, context);
    }

    const Type elementType = elements.front().type;
    const Type arrayType = Type::array(elementType);
    const std::size_t count = elements.size();
    const std::size_t totalBytes = 8 + count * elementSizeInBytes(elementType);

    const std::string base =
        memory_.emitCheckedMalloc(std::to_string(totalBytes), emitter, context);
    emitter.line("store i64 " + std::to_string(count) + ", ptr " + base);

    const std::string elems = emitter.freshTemp();
    emitter.line(elems + " = getelementptr inbounds i8, ptr " + base + ", i64 8");

    for (std::size_t index{}; index < count; ++index) {
      const Value indexValue{std::to_string(index), Type::i32()};
      const std::string slot = memory_.emitRawBufferElementPointer(
          Value{elems, Type::rawPtr()}, indexValue, elementType, emitter);
      memory_.emitBufferStore(elementType, elements[index].text, slot, emitter);
    }

    return Value{base, arrayType, true};
  }

  Value ExpressionEmitter::generateIndexExpression(const ast::IndexExpression& index,
                                                   IREmitter& emitter,
                                                   FunctionCodegenContext& context,
                                                   LLVMGenerator::OwnershipMode ownership) const {
    const Value base = generateRvalue(*index.base, emitter, context, std::nullopt,
                                      LLVMGenerator::OwnershipMode::Borrow);
    const Value indexValue = generateRvalue(*index.index, emitter, context);

    if (index.standardContainer) {
      const StandardContainer container = index.standardContainer->first;
      const std::vector<Type>& typeArgs = index.standardContainer->second.typeArguments();
      if (container == StandardContainer::Sequence) {
        return emitStandardContainerCall(container, ContainerOperation::Get, typeArgs,
                                         {base, indexValue}, emitter, context);
      }
      if (container == StandardContainer::Set) {
        return emitStandardContainerCall(container, ContainerOperation::Contains, typeArgs,
                                         {base, indexValue}, emitter, context);
      }

      const Value contains = emitStandardContainerCall(
          container, ContainerOperation::Contains, typeArgs, {base, indexValue}, emitter, context);
      const int labelId = emitter.freshLabelId();
      const std::string present = "dictionary.index.present" + std::to_string(labelId);
      const std::string missing = "dictionary.index.missing" + std::to_string(labelId);
      const std::string ready = "dictionary.index.ready" + std::to_string(labelId);
      emitter.emitCondBranch(contains.text, present, missing);

      emitter.emitLabel(present);
      emitter.emitBranch(ready);

      emitter.emitLabel(missing);
      const Value defaultValue = module().emitDefaultValue(typeArgs[1], emitter, context);
      (void)emitStandardContainerCall(container, ContainerOperation::Insert, typeArgs,
                                      {base, indexValue, defaultValue}, emitter, context);
      emitter.emitBranch(ready);

      emitter.emitLabel(ready);
      return emitStandardContainerCall(container, ContainerOperation::Get, typeArgs,
                                       {base, indexValue}, emitter, context);
    }

    if (base.type.kind() == TypeKind::Array) {
      const Type elementType = base.type.elementType();
      const std::string pointer =
          memory_.emitArrayElementPointer(base, indexValue, elementType, emitter, context);
      const std::string result = memory_.emitBufferLoad(elementType, pointer, emitter);
      Value loaded{result, elementType, false};
      if (ownership_.typeNeedsDrop(elementType, context) &&
          (ownership == LLVMGenerator::OwnershipMode::Own || base.owned)) {
        loaded = ownership_.emitCloneValue(loaded, emitter, context);
      }
      ownership_.emitReleaseIfOwned(base, emitter, context);
      return loaded;
    }

    const std::string length = emitter.freshTemp();
    emitter.line(length + " = call i64 @strlen(ptr " + base.text + ")");
    memory_.emitBoundsCheck(length, indexValue, emitter, context, "string index out of bounds\n");

    const std::string pointer = emitter.freshTemp();
    emitter.line(pointer + " = getelementptr inbounds i8, ptr " + base.text + ", i32 " +
                 indexValue.text);
    const std::string byte = emitter.freshTemp();
    emitter.line(byte + " = load i8, ptr " + pointer);
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = zext i8 " + byte + " to i32");
    ownership_.emitReleaseIfOwned(base, emitter, context);
    return Value{result, Type::i32()};
  }

  Value ExpressionEmitter::emitCheckedF64ToI32Cast(const Value& source, IREmitter& emitter,
                                                   FunctionCodegenContext& context) const {
    constexpr double minimumInput =
        static_cast<double>(std::numeric_limits<std::int32_t>::min()) - 1.0;
    constexpr double maximumInput =
        static_cast<double>(std::numeric_limits<std::int32_t>::max()) + 1.0;

    const std::string aboveMinimum = emitter.freshTemp();
    emitter.line(aboveMinimum + " = fcmp ogt double " + source.text + ", " +
                 formatLLVMFloatLiteral(minimumInput));
    const std::string belowMaximum = emitter.freshTemp();
    emitter.line(belowMaximum + " = fcmp olt double " + source.text + ", " +
                 formatLLVMFloatLiteral(maximumInput));
    const std::string inRange = emitter.freshTemp();
    emitter.line(inRange + " = and i1 " + aboveMinimum + ", " + belowMaximum);
    memory_.emitTrapUnless(inRange, "cast", emitter, context, "invalid f64 to i32 cast\n");

    const std::string result = emitter.freshTemp();
    emitter.line(result + " = fptosi double " + source.text + " to i32");
    return Value{result, Type::i32()};
  }

  Value ExpressionEmitter::generateCastExpression(const ast::CastExpression& cast,
                                                  IREmitter& emitter,
                                                  FunctionCodegenContext& context) const {
    const Value source = generateRvalue(*cast.expression, emitter, context);
    const Type targetType = cast.targetType;

    if (source.type == targetType)
      return source;

    if (source.type == Type::i32() && targetType == Type::f64()) {
      const std::string result = emitter.freshTemp();
      emitter.line(result + " = sitofp i32 " + source.text + " to double");
      return Value{result, Type::f64()};
    }

    if (source.type == Type::f64() && targetType == Type::i32()) {
      return emitCheckedF64ToI32Cast(source, emitter, context);
    }

    if (source.type == Type::boolean() && targetType == Type::i32()) {
      const std::string result = emitter.freshTemp();
      emitter.line(result + " = zext i1 " + source.text + " to i32");
      return Value{result, Type::i32()};
    }

    if (source.type == Type::i32() && targetType == Type::boolean()) {
      const std::string result = emitter.freshTemp();
      emitter.line(result + " = icmp ne i32 " + source.text + ", 0");
      return Value{result, Type::boolean()};
    }

    throw CompileError("codegen: unsupported cast");
  }

  std::string ExpressionEmitter::generateCondition(const ast::Expression& expression,
                                                   IREmitter& emitter,
                                                   FunctionCodegenContext& context) const {
    const Value value = generateRvalue(expression, emitter, context);
    if (value.type != Type::boolean())
      throw CompileError("codegen: condition must be bool");
    return value.text;
  }

  Value ExpressionEmitter::generateRvalue(const ast::Expression& expression, IREmitter& emitter,
                                          FunctionCodegenContext& context,
                                          std::optional<Type> expectedType,
                                          LLVMGenerator::OwnershipMode ownership) const {
    ExpressionVisitor visitor(*this, emitter, context, std::move(expectedType), ownership);
    expression.accept(visitor);
    return visitor.result();
  }

  Value ExpressionEmitter::generateBinaryExpression(const ast::BinaryExpression& binary,
                                                    IREmitter& emitter,
                                                    FunctionCodegenContext& context) const {

    const BinaryOperatorInfo* info = binaryOperatorInfo(binary.op);
    if (info == nullptr) {
      throw CompileError("codegen: internal error: unknown binary operator");
    }

    if (info->shortCircuit) {
      return generateShortCircuitBinaryExpression(binary, emitter, context);
    }

    const Value left = generateRvalue(*binary.left, emitter, context, std::nullopt,
                                      LLVMGenerator::OwnershipMode::Borrow);
    const Value right = generateRvalue(*binary.right, emitter, context, std::nullopt,
                                       LLVMGenerator::OwnershipMode::Borrow);

    if (binary.op == ast::BinaryOperator::Add && left.type == Type::str() &&
        right.type == Type::str()) {
      return generateStringConcatExpression(left, right, emitter, context);
    }

    if (binary.op == ast::BinaryOperator::Add &&
        (left.type.kind() == TypeKind::Array || right.type.kind() == TypeKind::Array ||
         left.type.kind() == TypeKind::Struct || right.type.kind() == TypeKind::Struct)) {
      return generateCollectionAddExpression(left, right, emitter, context);
    }

    if (info->comparison) {
      const Value result = generateComparisonExpression(binary, left, right, emitter);
      ownership_.emitReleaseIfOwned(left, emitter, context);
      ownership_.emitReleaseIfOwned(right, emitter, context);
      return result;
    }

    const Value result = generateNumericBinaryExpression(binary, left, right, emitter, context);
    ownership_.emitReleaseIfOwned(left, emitter, context);
    ownership_.emitReleaseIfOwned(right, emitter, context);
    return result;
  }

  Value
  ExpressionEmitter::generateShortCircuitBinaryExpression(const ast::BinaryExpression& binary,
                                                          IREmitter& emitter,
                                                          FunctionCodegenContext& context) const {
    const Value left = generateRvalue(*binary.left, emitter, context);
    const int labelId = emitter.freshLabelId();
    const std::string shortCircuitLabel =
        (binary.op == ast::BinaryOperator::And ? "and.short" : "or.short") +
        std::to_string(labelId);
    const std::string rhsLabel =
        (binary.op == ast::BinaryOperator::And ? "and.rhs" : "or.rhs") + std::to_string(labelId);
    const std::string mergeLabel =
        (binary.op == ast::BinaryOperator::And ? "and.end" : "or.end") + std::to_string(labelId);
    const std::string shortCircuitValue = binary.op == ast::BinaryOperator::And ? "false" : "true";

    if (binary.op == ast::BinaryOperator::And)
      emitter.emitCondBranch(left.text, rhsLabel, shortCircuitLabel);
    else
      emitter.emitCondBranch(left.text, shortCircuitLabel, rhsLabel);

    emitter.emitLabel(shortCircuitLabel);
    emitter.emitBranch(mergeLabel);

    emitter.emitLabel(rhsLabel);
    const Value right = generateRvalue(*binary.right, emitter, context);
    const std::string rhsJoinLabel =
        (binary.op == ast::BinaryOperator::And ? "and.rhs.join" : "or.rhs.join") +
        std::to_string(labelId);
    emitter.emitBranch(rhsJoinLabel);
    emitter.emitLabel(rhsJoinLabel);
    emitter.emitBranch(mergeLabel);

    // The phi selects the skipped value from the short-circuit edge, or the RHS from its join.
    emitter.emitLabel(mergeLabel);
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = phi i1 [ " + shortCircuitValue + ", %" + shortCircuitLabel +
                 " ], [ " + right.text + ", %" + rhsJoinLabel + " ]");
    return Value{result, Type::boolean()};
  }

  Value ExpressionEmitter::generateStringConcatExpression(const Value& left, const Value& right,
                                                          IREmitter& emitter,
                                                          FunctionCodegenContext& context) const {
    const std::string leftLength = emitter.freshTemp();
    emitter.line(leftLength + " = call i64 @strlen(ptr " + left.text + ")");
    const std::string rightLength = emitter.freshTemp();
    emitter.line(rightLength + " = call i64 @strlen(ptr " + right.text + ")");
    const std::string sumLength = emitter.freshTemp();
    emitter.line(sumLength + " = add i64 " + leftLength + ", " + rightLength);
    const std::string payloadSize = emitter.freshTemp();
    emitter.line(payloadSize + " = add i64 " + sumLength + ", 1");
    const std::string size = emitter.freshTemp();
    emitter.line(size + " = add i64 " + payloadSize + ", 4");
    const std::string allocation = memory_.emitCheckedMalloc(size, emitter, context);
    emitter.line("store i32 1, ptr " + allocation);
    const std::string buffer = emitter.freshTemp();
    emitter.line(buffer + " = getelementptr i8, ptr " + allocation + ", i64 4");
    emitter.line("call ptr @strcpy(ptr " + buffer + ", ptr " + left.text + ")");
    emitter.line("call ptr @strcat(ptr " + buffer + ", ptr " + right.text + ")");
    ownership_.emitReleaseIfOwned(left, emitter, context);
    ownership_.emitReleaseIfOwned(right, emitter, context);
    return Value{buffer, Type::str(), true};
  }

  Value ExpressionEmitter::generateCollectionAddExpression(const Value& left, const Value& right,
                                                           IREmitter& emitter,
                                                           FunctionCodegenContext& context) const {
    if (left.type.kind() == TypeKind::Array && right.type.kind() == TypeKind::Array) {
      return generateArrayAddExpression(left, right, emitter, context);
    }
    if (left.type.kind() == TypeKind::Struct && right.type.kind() == TypeKind::Struct) {
      return generateSequenceAddExpression(left, right, emitter, context);
    }
    throw CompileError("codegen: internal error: invalid collection addition operands");
  }

  Value ExpressionEmitter::generateArrayAddExpression(const Value& left, const Value& right,
                                                      IREmitter& emitter,
                                                      FunctionCodegenContext& context) const {
    if (left.type != right.type) {
      throw CompileError("codegen: internal error: invalid array addition operands");
    }

    const Type elementType = left.type.elementType();
    const std::string leftLength = emitter.freshTemp();
    emitter.line(leftLength + " = load i64, ptr " + left.text);
    const std::string rightLength = emitter.freshTemp();
    emitter.line(rightLength + " = load i64, ptr " + right.text);
    const std::string sameLength = emitter.freshTemp();
    emitter.line(sameLength + " = icmp eq i64 " + leftLength + ", " + rightLength);
    memory_.emitTrapUnless(sameLength, "array.add.length", emitter, context,
                           "array addition requires equal lengths\n");

    const std::string payloadBytes = emitter.freshTemp();
    emitter.line(payloadBytes + " = mul i64 " + leftLength + ", " +
                 std::to_string(elementSizeInBytes(elementType)));
    const std::string totalBytes = emitter.freshTemp();
    emitter.line(totalBytes + " = add i64 " + payloadBytes + ", 8");
    const std::string result = memory_.emitCheckedMalloc(totalBytes, emitter, context);
    emitter.line("store i64 " + leftLength + ", ptr " + result);

    const std::string leftElements = emitter.freshTemp();
    emitter.line(leftElements + " = getelementptr inbounds i8, ptr " + left.text + ", i64 8");
    const std::string rightElements = emitter.freshTemp();
    emitter.line(rightElements + " = getelementptr inbounds i8, ptr " + right.text + ", i64 8");
    const std::string resultElements = emitter.freshTemp();
    emitter.line(resultElements + " = getelementptr inbounds i8, ptr " + result + ", i64 8");

    const int labelId = emitter.freshLabelId();
    const std::string indexSlot = emitter.freshTemp();
    emitter.line(indexSlot + " = alloca i64");
    emitter.line("store i64 0, ptr " + indexSlot);
    const std::string conditionLabel = "array.add.cond" + std::to_string(labelId);
    const std::string bodyLabel = "array.add.body" + std::to_string(labelId);
    const std::string endLabel = "array.add.end" + std::to_string(labelId);
    emitter.emitBranch(conditionLabel);
    emitter.emitLabel(conditionLabel);
    const std::string index64 = emitter.freshTemp();
    emitter.line(index64 + " = load i64, ptr " + indexSlot);
    const std::string inRange = emitter.freshTemp();
    emitter.line(inRange + " = icmp ult i64 " + index64 + ", " + leftLength);
    emitter.emitCondBranch(inRange, bodyLabel, endLabel);

    emitter.emitLabel(bodyLabel);
    const std::string index = emitter.freshTemp();
    emitter.line(index + " = trunc i64 " + index64 + " to i32");
    const Value indexValue{index, Type::i32()};
    const std::string leftPointer = memory_.emitRawBufferElementPointer(
        Value{leftElements, Type::rawPtr()}, indexValue, elementType, emitter);
    const std::string rightPointer = memory_.emitRawBufferElementPointer(
        Value{rightElements, Type::rawPtr()}, indexValue, elementType, emitter);
    const Value leftElement{memory_.emitBufferLoad(elementType, leftPointer, emitter), elementType};
    const Value rightElement{memory_.emitBufferLoad(elementType, rightPointer, emitter),
                             elementType};
    const Value sum = generateElementAddExpression(leftElement, rightElement, emitter, context);
    const std::string resultPointer = memory_.emitRawBufferElementPointer(
        Value{resultElements, Type::rawPtr()}, indexValue, elementType, emitter);
    memory_.emitBufferStore(elementType, sum.text, resultPointer, emitter);
    const std::string nextIndex = emitter.freshTemp();
    emitter.line(nextIndex + " = add i64 " + index64 + ", 1");
    emitter.line("store i64 " + nextIndex + ", ptr " + indexSlot);
    emitter.emitBranch(conditionLabel);
    emitter.emitLabel(endLabel);
    ownership_.emitReleaseIfOwned(left, emitter, context);
    ownership_.emitReleaseIfOwned(right, emitter, context);
    return Value{result, left.type, true};
  }

  Value ExpressionEmitter::generateSequenceAddExpression(const Value& left, const Value& right,
                                                         IREmitter& emitter,
                                                         FunctionCodegenContext& context) const {
    const auto specialization =
        context.module.structSpecializationTypeArgs.find(left.type.structName());
    if (left.type != right.type ||
        specialization == context.module.structSpecializationTypeArgs.end() ||
        specialization->second.size() != 2 ||
        specialization->second[1].kind() != TypeKind::ImplTag) {
      throw CompileError("codegen: internal error: invalid sequence addition operands");
    }

    const Type elementType = specialization->second[0];
    const ImplementationTag tag = specialization->second[1].implementationTagValue();
    if (tag != ImplementationTag::Arr && tag != ImplementationTag::List) {
      throw CompileError("codegen: internal error: unsupported sequence implementation");
    }

    const std::string leftHandle = emitter.freshTemp();
    emitter.line(leftHandle + " = extractvalue " + LLVMType(left.type) + " " + left.text + ", 0");
    const std::string rightHandle = emitter.freshTemp();
    emitter.line(rightHandle + " = extractvalue " + LLVMType(right.type) + " " + right.text +
                 ", 0");

    const std::string leftLengthPointer = emitter.freshTemp();
    const std::string rightLengthPointer = emitter.freshTemp();
    const int lengthOffset = tag == ImplementationTag::Arr ? 0 : 16;
    emitter.line(leftLengthPointer + " = getelementptr i8, ptr " + leftHandle + ", i32 " +
                 std::to_string(lengthOffset));
    emitter.line(rightLengthPointer + " = getelementptr i8, ptr " + rightHandle + ", i32 " +
                 std::to_string(lengthOffset));
    const std::string leftLength = emitter.freshTemp();
    emitter.line(leftLength + " = load i32, ptr " + leftLengthPointer);
    const std::string rightLength = emitter.freshTemp();
    emitter.line(rightLength + " = load i32, ptr " + rightLengthPointer);
    const std::string sameLength = emitter.freshTemp();
    emitter.line(sameLength + " = icmp eq i32 " + leftLength + ", " + rightLength);
    memory_.emitTrapUnless(sameLength, "sequence.add.length", emitter, context,
                           "sequence addition requires equal lengths\n");

    const int labelId = emitter.freshLabelId();
    const std::string indexSlot = emitter.freshTemp();
    emitter.line(indexSlot + " = alloca i32");
    emitter.line("store i32 0, ptr " + indexSlot);
    const std::string conditionLabel = "sequence.add.cond" + std::to_string(labelId);
    const std::string bodyLabel = "sequence.add.body" + std::to_string(labelId);
    const std::string endLabel = "sequence.add.end" + std::to_string(labelId);

    std::string resultHandle;
    std::string leftData;
    std::string rightData;
    std::string leftNodeSlot;
    std::string rightNodeSlot;
    if (tag == ImplementationTag::Arr) {
      resultHandle = memory_.emitCheckedMalloc("16", emitter, context);
      const std::string needsMinimumCapacity = emitter.freshTemp();
      emitter.line(needsMinimumCapacity + " = icmp slt i32 " + leftLength + ", 4");
      const std::string capacity = emitter.freshTemp();
      emitter.line(capacity + " = select i1 " + needsMinimumCapacity + ", i32 4, i32 " +
                   leftLength);
      const std::string dataBytes = emitter.freshTemp();
      emitter.line(dataBytes + " = mul i32 " + capacity + ", " +
                   std::to_string(elementSizeInBytes(elementType)));
      const std::string dataBytes64 = emitter.freshTemp();
      emitter.line(dataBytes64 + " = sext i32 " + dataBytes + " to i64");
      const std::string resultData = memory_.emitCheckedMalloc(dataBytes64, emitter, context);
      emitter.line("store i32 " + leftLength + ", ptr " + resultHandle);
      const std::string resultCapacityPointer = emitter.freshTemp();
      emitter.line(resultCapacityPointer + " = getelementptr i8, ptr " + resultHandle + ", i32 4");
      emitter.line("store i32 " + capacity + ", ptr " + resultCapacityPointer);
      const std::string resultDataPointer = emitter.freshTemp();
      emitter.line(resultDataPointer + " = getelementptr i8, ptr " + resultHandle + ", i32 8");
      emitter.line("store ptr " + resultData + ", ptr " + resultDataPointer);

      const std::string leftDataPointer = emitter.freshTemp();
      emitter.line(leftDataPointer + " = getelementptr i8, ptr " + leftHandle + ", i32 8");
      leftData = emitter.freshTemp();
      emitter.line(leftData + " = load ptr, ptr " + leftDataPointer);
      const std::string rightDataPointer = emitter.freshTemp();
      emitter.line(rightDataPointer + " = getelementptr i8, ptr " + rightHandle + ", i32 8");
      rightData = emitter.freshTemp();
      emitter.line(rightData + " = load ptr, ptr " + rightDataPointer);
      emitter.emitBranch(conditionLabel);

      emitter.emitLabel(conditionLabel);
      const std::string index = emitter.freshTemp();
      emitter.line(index + " = load i32, ptr " + indexSlot);
      const std::string inRange = emitter.freshTemp();
      emitter.line(inRange + " = icmp slt i32 " + index + ", " + leftLength);
      emitter.emitCondBranch(inRange, bodyLabel, endLabel);
      emitter.emitLabel(bodyLabel);
      const Value indexValue{index, Type::i32()};
      const std::string leftPointer = memory_.emitRawBufferElementPointer(
          Value{leftData, Type::rawPtr()}, indexValue, elementType, emitter);
      const std::string rightPointer = memory_.emitRawBufferElementPointer(
          Value{rightData, Type::rawPtr()}, indexValue, elementType, emitter);
      const Value leftElement{memory_.emitBufferLoad(elementType, leftPointer, emitter),
                              elementType};
      const Value rightElement{memory_.emitBufferLoad(elementType, rightPointer, emitter),
                               elementType};
      const Value sum = generateElementAddExpression(leftElement, rightElement, emitter, context);
      const std::string resultPointer = memory_.emitRawBufferElementPointer(
          Value{resultData, Type::rawPtr()}, indexValue, elementType, emitter);
      memory_.emitBufferStore(elementType, sum.text, resultPointer, emitter);
      const std::string nextIndex = emitter.freshTemp();
      emitter.line(nextIndex + " = add i32 " + index + ", 1");
      emitter.line("store i32 " + nextIndex + ", ptr " + indexSlot);
      emitter.emitBranch(conditionLabel);
      emitter.emitLabel(endLabel);
    } else {
      resultHandle = memory_.emitCheckedMalloc("20", emitter, context);
      emitter.line("store ptr " + resultHandle + ", ptr " + resultHandle);
      const std::string resultNextPointer = emitter.freshTemp();
      emitter.line(resultNextPointer + " = getelementptr i8, ptr " + resultHandle + ", i32 8");
      emitter.line("store ptr " + resultHandle + ", ptr " + resultNextPointer);
      const std::string resultLengthPointer = emitter.freshTemp();
      emitter.line(resultLengthPointer + " = getelementptr i8, ptr " + resultHandle + ", i32 16");
      emitter.line("store i32 0, ptr " + resultLengthPointer);

      const std::string leftFirstPointer = emitter.freshTemp();
      emitter.line(leftFirstPointer + " = getelementptr i8, ptr " + leftHandle + ", i32 8");
      const std::string leftFirst = emitter.freshTemp();
      emitter.line(leftFirst + " = load ptr, ptr " + leftFirstPointer);
      const std::string rightFirstPointer = emitter.freshTemp();
      emitter.line(rightFirstPointer + " = getelementptr i8, ptr " + rightHandle + ", i32 8");
      const std::string rightFirst = emitter.freshTemp();
      emitter.line(rightFirst + " = load ptr, ptr " + rightFirstPointer);
      leftNodeSlot = emitter.freshTemp();
      rightNodeSlot = emitter.freshTemp();
      emitter.line(leftNodeSlot + " = alloca ptr");
      emitter.line(rightNodeSlot + " = alloca ptr");
      emitter.line("store ptr " + leftFirst + ", ptr " + leftNodeSlot);
      emitter.line("store ptr " + rightFirst + ", ptr " + rightNodeSlot);
      emitter.emitBranch(conditionLabel);

      emitter.emitLabel(conditionLabel);
      const std::string index = emitter.freshTemp();
      emitter.line(index + " = load i32, ptr " + indexSlot);
      const std::string inRange = emitter.freshTemp();
      emitter.line(inRange + " = icmp slt i32 " + index + ", " + leftLength);
      emitter.emitCondBranch(inRange, bodyLabel, endLabel);
      emitter.emitLabel(bodyLabel);
      const std::string leftNode = emitter.freshTemp();
      emitter.line(leftNode + " = load ptr, ptr " + leftNodeSlot);
      const std::string rightNode = emitter.freshTemp();
      emitter.line(rightNode + " = load ptr, ptr " + rightNodeSlot);
      const Value valueIndex{std::to_string(16 / elementSizeInBytes(elementType)), Type::i32()};
      const std::string leftPointer = memory_.emitRawBufferElementPointer(
          Value{leftNode, Type::rawPtr()}, valueIndex, elementType, emitter);
      const std::string rightPointer = memory_.emitRawBufferElementPointer(
          Value{rightNode, Type::rawPtr()}, valueIndex, elementType, emitter);
      const Value leftElement{memory_.emitBufferLoad(elementType, leftPointer, emitter),
                              elementType};
      const Value rightElement{memory_.emitBufferLoad(elementType, rightPointer, emitter),
                               elementType};
      const Value sum = generateElementAddExpression(leftElement, rightElement, emitter, context);

      const std::string newNode = memory_.emitCheckedMalloc(
          std::to_string(16 + elementSizeInBytes(elementType)), emitter, context);
      const std::string newValuePointer = memory_.emitRawBufferElementPointer(
          Value{newNode, Type::rawPtr()}, valueIndex, elementType, emitter);
      memory_.emitBufferStore(elementType, sum.text, newValuePointer, emitter);
      const std::string resultLast = emitter.freshTemp();
      emitter.line(resultLast + " = load ptr, ptr " + resultHandle);
      emitter.line("store ptr " + resultLast + ", ptr " + newNode);
      const std::string newNextPointer = emitter.freshTemp();
      emitter.line(newNextPointer + " = getelementptr i8, ptr " + newNode + ", i32 8");
      emitter.line("store ptr " + resultHandle + ", ptr " + newNextPointer);
      const std::string resultLastNextPointer = emitter.freshTemp();
      emitter.line(resultLastNextPointer + " = getelementptr i8, ptr " + resultLast + ", i32 8");
      emitter.line("store ptr " + newNode + ", ptr " + resultLastNextPointer);
      emitter.line("store ptr " + newNode + ", ptr " + resultHandle);

      const std::string leftNextPointer = emitter.freshTemp();
      emitter.line(leftNextPointer + " = getelementptr i8, ptr " + leftNode + ", i32 8");
      const std::string leftNext = emitter.freshTemp();
      emitter.line(leftNext + " = load ptr, ptr " + leftNextPointer);
      emitter.line("store ptr " + leftNext + ", ptr " + leftNodeSlot);
      const std::string rightNextPointer = emitter.freshTemp();
      emitter.line(rightNextPointer + " = getelementptr i8, ptr " + rightNode + ", i32 8");
      const std::string rightNext = emitter.freshTemp();
      emitter.line(rightNext + " = load ptr, ptr " + rightNextPointer);
      emitter.line("store ptr " + rightNext + ", ptr " + rightNodeSlot);
      const std::string nextIndex = emitter.freshTemp();
      emitter.line(nextIndex + " = add i32 " + index + ", 1");
      emitter.line("store i32 " + nextIndex + ", ptr " + indexSlot);
      emitter.emitBranch(conditionLabel);
      emitter.emitLabel(endLabel);
      emitter.line("store i32 " + leftLength + ", ptr " + resultLengthPointer);
    }

    const std::string result = emitter.freshTemp();
    emitter.line(result + " = insertvalue " + LLVMType(left.type) + " undef, ptr " + resultHandle +
                 ", 0");
    return Value{result, left.type};
  }

  Value ExpressionEmitter::generateElementAddExpression(const Value& left, const Value& right,
                                                        IREmitter& emitter,
                                                        FunctionCodegenContext& context) const {
    if (left.type != right.type) {
      throw CompileError("codegen: internal error: mismatched collection element types");
    }
    if (left.type.kind() == TypeKind::Array) {
      return generateArrayAddExpression(left, right, emitter, context);
    }
    if (left.type == Type::str()) {
      return generateStringConcatExpression(left, right, emitter, context);
    }
    const std::string result = emitter.freshTemp();
    if (left.type == Type::f64()) {
      emitter.line(result + " = fadd double " + left.text + ", " + right.text);
      return Value{result, Type::f64()};
    }
    if (left.type == Type::i32()) {
      emitter.line(result + " = add i32 " + left.text + ", " + right.text);
      return Value{result, Type::i32()};
    }
    throw CompileError("codegen: internal error: unsupported collection element addition");
  }

  Value ExpressionEmitter::generateComparisonExpression(const ast::BinaryExpression& binary,
                                                        const Value& left, const Value& right,
                                                        IREmitter& emitter) const {
    const BinaryOperatorInfo* info = binaryOperatorInfo(binary.op);
    if (info == nullptr) {
      throw CompileError("codegen: internal error: unknown comparison operator");
    }

    const std::string result = emitter.freshTemp();
    if (left.type == Type::f64() && right.type == Type::f64()) {
      emitter.line(result + " = fcmp " + std::string(info->LLVMFloatPredicate) + " double " +
                   left.text + ", " + right.text);
      return Value{result, Type::boolean()};
    }

    if (left.type == Type::str() && right.type == Type::str()) {
      const std::string compared = emitter.freshTemp();
      emitter.line(compared + " = call i32 @strcmp(ptr " + left.text + ", ptr " + right.text + ")");
      emitter.line(result + " = icmp " + std::string(info->LLVMIntegerPredicate) + " i32 " +
                   compared + ", 0");
      return Value{result, Type::boolean()};
    }

    const std::string integerType = left.type == Type::boolean() ? "i1" : "i32";
    emitter.line(result + " = icmp " + std::string(info->LLVMIntegerPredicate) + " " + integerType +
                 " " + left.text + ", " + right.text);
    return Value{result, Type::boolean()};
  }

  Value ExpressionEmitter::generateNumericBinaryExpression(const ast::BinaryExpression& binary,
                                                           const Value& left, const Value& right,
                                                           IREmitter& emitter,
                                                           FunctionCodegenContext& context) const {
    const BinaryOperatorInfo* info = binaryOperatorInfo(binary.op);
    if (info == nullptr) {
      throw CompileError("codegen: internal error: unknown numeric operator");
    }

    const std::string result = emitter.freshTemp();
    if (left.type == Type::f64() && right.type == Type::f64()) {
      emitter.line(result + " = " + std::string(info->LLVMFloatInstruction) + " double " +
                   left.text + ", " + right.text);
      return Value{result, Type::f64()};
    }

    if (info->integerSafetyRule == IntegerSafetyRule::SignedDivisionOrRemainder) {
      const bool division = binary.op == ast::BinaryOperator::Divide;
      const std::string operation = division ? "division" : "remainder";
      const std::string divisorNonZero = emitter.freshTemp();
      emitter.line(divisorNonZero + " = icmp ne i32 " + right.text + ", 0");
      memory_.emitTrapUnless(divisorNonZero, "integer.divisor", emitter, context,
                             "integer " + operation + " by zero\n");

      const std::string leftIsMin = emitter.freshTemp();
      emitter.line(leftIsMin + " = icmp eq i32 " + left.text + ", " +
                   std::to_string(std::numeric_limits<std::int32_t>::min()));
      const std::string rightIsNegativeOne = emitter.freshTemp();
      emitter.line(rightIsNegativeOne + " = icmp eq i32 " + right.text + ", -1");
      const std::string overflows = emitter.freshTemp();
      emitter.line(overflows + " = and i1 " + leftIsMin + ", " + rightIsNegativeOne);
      const std::string noOverflow = emitter.freshTemp();
      emitter.line(noOverflow + " = xor i1 " + overflows + ", true");
      memory_.emitTrapUnless(noOverflow, "integer.overflow", emitter, context,
                             "integer " + operation + " overflow\n");
    } else if (info->integerSafetyRule == IntegerSafetyRule::ShiftCount) {
      const std::string countInRange = emitter.freshTemp();
      emitter.line(countInRange + " = icmp ult i32 " + right.text + ", 32");
      memory_.emitTrapUnless(countInRange, "integer.shift", emitter, context,
                             "integer shift count out of range (expected 0..31)\n");
    }

    emitter.line(result + " = " + std::string(info->LLVMIntegerInstruction) + " i32 " + left.text +
                 ", " + right.text);
    return Value{result, Type::i32()};
  }

} // namespace noria::codegen_detail
