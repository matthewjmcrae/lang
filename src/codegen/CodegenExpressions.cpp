#include "CodegenInternal.hpp"

#include "noria/Builtins.hpp"
#include "noria/Diagnostic.hpp"
#include "noria/Runtime.hpp"
#include "noria/SemanticTables.hpp"

#include "CodegenSupport.hpp"
#include <array>
#include <charconv>
#include <limits>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace noria {

  using namespace codegen_detail;

  LLVMGenerator::Impl::ExpressionVisitor::ExpressionVisitor(const LLVMGenerator::Impl& generator,
                                                            IREmitter& emitter,
                                                            FunctionCodegenContext& context,
                                                            const std::vector<Scope>& scopes)
      : ExpressionOnlyVisitor("codegen"), generator_(generator), emitter_(emitter),
        context_(context), scopes_(scopes) {}

  void LLVMGenerator::Impl::ExpressionVisitor::visit(const ast::IntegerLiteral& integer) {
    result_ = Value{std::to_string(integer.value), Type::i32()};
  }

  void LLVMGenerator::Impl::ExpressionVisitor::visit(const ast::FloatLiteral& floating) {
    result_ = Value{formatLLVMFloatLiteral(floating.value), Type::f64()};
  }

  void LLVMGenerator::Impl::ExpressionVisitor::visit(const ast::StringLiteral& stringLiteral) {
    result_ = generator_.generateStringLiteral(stringLiteral, emitter_, context_);
  }

  void LLVMGenerator::Impl::ExpressionVisitor::visit(const ast::BoolLiteral& boolean) {
    result_ = Value{boolean.value ? "true" : "false", Type::boolean()};
  }

  void LLVMGenerator::Impl::ExpressionVisitor::visit(const ast::UnaryExpression& unary) {
    const Value operand = generator_.generateRvalue(*unary.operand, emitter_, context_, scopes_);
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

  void LLVMGenerator::Impl::ExpressionVisitor::visit(const ast::CastExpression& castExpression) {
    result_ = generator_.generateCastExpression(castExpression, emitter_, context_, scopes_);
  }

  void LLVMGenerator::Impl::ExpressionVisitor::visit(const ast::BinaryExpression& binary) {
    result_ = generator_.generateBinaryExpression(binary, emitter_, context_, scopes_);
  }

  void LLVMGenerator::Impl::ExpressionVisitor::visit(const ast::IdentifierExpression& identifier) {
    const LocalBinding& local = generator_.lookupLocal(scopes_, identifier.name);

    const std::string result = emitter_.freshTemp();
    emitter_.emitLoad(local.type, local.slot, result);
    result_ = Value{result, local.type};
  }

  void LLVMGenerator::Impl::ExpressionVisitor::visit(const ast::CallExpression& call) {
    if (auto builtin = generator_.tryGenerateBuiltinCall(call, emitter_, context_, scopes_)) {
      result_ = *builtin;
      return;
    }

    const auto function = context_.functions.find(call.callee);
    if (function == context_.functions.end())
      throw CompileError("codegen: unknown function '" + call.callee + "'");

    std::vector<Value> arguments;
    arguments.reserve(call.arguments.size());

    for (const auto& argument : call.arguments) {
      arguments.push_back(generator_.generateRvalue(*argument, emitter_, context_, scopes_));
    }

    const std::string result = emitter_.freshTemp();
    std::string callLine =
        result + " = call " + LLVMType(function->second.returnType) + " @" + call.callee + "(";
    for (std::size_t index{}; index < arguments.size(); ++index) {
      if (index != 0)
        callLine += ", ";

      callLine += LLVMType(arguments[index].type) + " " + arguments[index].text;
    }
    callLine += ")";
    emitter_.line(callLine);
    result_ = Value{result, function->second.returnType};
  }

  void LLVMGenerator::Impl::ExpressionVisitor::visit(const ast::ArrayLiteral& literal) {
    result_ = generator_.generateArrayLiteral(literal, emitter_, context_, scopes_);
  }

  void LLVMGenerator::Impl::ExpressionVisitor::visit(const ast::IndexExpression& index) {
    result_ = generator_.generateIndexExpression(index, emitter_, context_, scopes_);
  }

  void LLVMGenerator::Impl::ExpressionVisitor::visit(const ast::StructLiteral& literal) {
    result_ = generator_.generateStructLiteral(literal, emitter_, context_, scopes_);
  }

  void LLVMGenerator::Impl::ExpressionVisitor::visit(const ast::FieldAccessExpression& access) {
    result_ = generator_.generateFieldAccess(access, emitter_, context_, scopes_);
  }

  LLVMGenerator::Impl::Value
  LLVMGenerator::Impl::generateStringLiteral(const ast::StringLiteral& literal, IREmitter& emitter,
                                             FunctionCodegenContext& context) const {
    return Value{emitCStringPointer(literal.value, emitter, context), Type::str()};
  }

  LLVMGenerator::Impl::Value
  LLVMGenerator::Impl::generateArrayLiteral(const ast::ArrayLiteral& literal, IREmitter& emitter,
                                            FunctionCodegenContext& context,
                                            const std::vector<Scope>& scopes) const {
    std::vector<Value> elements;
    elements.reserve(literal.elements.size());

    for (const auto& element : literal.elements) {
      elements.push_back(generateRvalue(*element, emitter, context, scopes));
    }

    const Type elementType = elements.front().type;
    const Type arrayType = Type::array(elementType);
    const std::size_t count = elements.size();
    const std::size_t totalBytes = 8 + count * elementSizeInBytes(elementType);

    const std::string base = emitCheckedMalloc(std::to_string(totalBytes), emitter, context);
    emitter.line("store i64 " + std::to_string(count) + ", ptr " + base);

    const std::string elems = emitter.freshTemp();
    emitter.line(elems + " = getelementptr inbounds i8, ptr " + base + ", i64 8");

    for (std::size_t index{}; index < count; ++index) {
      const Value indexValue{std::to_string(index), Type::i32()};
      const std::string slot = emitRawBufferElementPointer(Value{elems, Type::rawPtr()}, indexValue,
                                                           elementType, emitter);
      emitBufferStore(elementType, elements[index].text, slot, emitter);
    }

    return Value{base, arrayType};
  }

  LLVMGenerator::Impl::Value
  LLVMGenerator::Impl::generateIndexExpression(const ast::IndexExpression& index,
                                               IREmitter& emitter, FunctionCodegenContext& context,
                                               const std::vector<Scope>& scopes) const {
    const Value base = generateRvalue(*index.base, emitter, context, scopes);
    const Value indexValue = generateRvalue(*index.index, emitter, context, scopes);

    if (base.type.kind == TypeKind::Array) {
      if (!base.type.element)
        throw CompileError("codegen: array type missing element type");

      const Type elementType = *base.type.element;
      const std::string pointer =
          emitArrayElementPointer(base, indexValue, elementType, emitter, context);
      const std::string result = emitBufferLoad(elementType, pointer, emitter);
      return Value{result, elementType};
    }

    const std::string length = emitter.freshTemp();
    emitter.line(length + " = call i64 @strlen(ptr " + base.text + ")");
    emitBoundsCheck(length, indexValue, emitter, context, "string index out of bounds\n");

    const std::string pointer = emitter.freshTemp();
    emitter.line(pointer + " = getelementptr inbounds i8, ptr " + base.text + ", i32 " +
                 indexValue.text);
    const std::string byte = emitter.freshTemp();
    emitter.line(byte + " = load i8, ptr " + pointer);
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = zext i8 " + byte + " to i32");
    return Value{result, Type::i32()};
  }

  LLVMGenerator::Impl::Value
  LLVMGenerator::Impl::generateCastExpression(const ast::CastExpression& cast, IREmitter& emitter,
                                              FunctionCodegenContext& context,
                                              const std::vector<Scope>& scopes) const {
    const Value source = generateRvalue(*cast.expression, emitter, context, scopes);
    const Type targetType = cast.targetType;

    if (source.type == targetType)
      return source;

    const std::string result = emitter.freshTemp();

    if (source.type == Type::i32() && targetType == Type::f64()) {
      emitter.line(result + " = sitofp i32 " + source.text + " to double");
      return Value{result, Type::f64()};
    }

    if (source.type == Type::f64() && targetType == Type::i32()) {
      emitter.line(result + " = fptosi double " + source.text + " to i32");
      return Value{result, Type::i32()};
    }

    if (source.type == Type::boolean() && targetType == Type::i32()) {
      emitter.line(result + " = zext i1 " + source.text + " to i32");
      return Value{result, Type::i32()};
    }

    if (source.type == Type::i32() && targetType == Type::boolean()) {
      emitter.line(result + " = icmp ne i32 " + source.text + ", 0");
      return Value{result, Type::boolean()};
    }

    throw CompileError("codegen: unsupported cast");
  }

  std::string LLVMGenerator::Impl::generateCondition(const ast::Expression& expression,
                                                     IREmitter& emitter,
                                                     FunctionCodegenContext& context,
                                                     const std::vector<Scope>& scopes) const {
    const Value value = generateRvalue(expression, emitter, context, scopes);
    if (value.type != Type::boolean())
      throw CompileError("codegen: condition must be bool");
    return value.text;
  }

  LLVMGenerator::Impl::Value
  LLVMGenerator::Impl::generateRvalue(const ast::Expression& expression, IREmitter& emitter,
                                      FunctionCodegenContext& context,
                                      const std::vector<Scope>& scopes) const {
    ExpressionVisitor visitor(*this, emitter, context, scopes);
    expression.accept(visitor);
    return visitor.result();
  }

  LLVMGenerator::Impl::Value
  LLVMGenerator::Impl::generateBinaryExpression(const ast::BinaryExpression& binary,
                                                IREmitter& emitter, FunctionCodegenContext& context,
                                                const std::vector<Scope>& scopes) const {

    const BinaryOperatorInfo* info = binaryOperatorInfo(binary.op);
    if (info == nullptr) {
      throw CompileError("codegen: internal error: unknown binary operator");
    }

    if (info->shortCircuit) {
      return generateShortCircuitBinaryExpression(binary, emitter, context, scopes);
    }

    const Value left = generateRvalue(*binary.left, emitter, context, scopes);
    const Value right = generateRvalue(*binary.right, emitter, context, scopes);

    if (binary.op == ast::BinaryOperator::Add && left.type == Type::str() &&
        right.type == Type::str()) {
      return generateStringConcatExpression(left, right, emitter, context);
    }

    if (info->comparison) {
      return generateComparisonExpression(binary, left, right, emitter);
    }

    return generateNumericBinaryExpression(binary, left, right, emitter);
  }

  LLVMGenerator::Impl::Value LLVMGenerator::Impl::generateShortCircuitBinaryExpression(
      const ast::BinaryExpression& binary, IREmitter& emitter, FunctionCodegenContext& context,
      const std::vector<Scope>& scopes) const {
    const Value left = generateRvalue(*binary.left, emitter, context, scopes);
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
    const Value right = generateRvalue(*binary.right, emitter, context, scopes);
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

  LLVMGenerator::Impl::Value
  LLVMGenerator::Impl::generateStringConcatExpression(const Value& left, const Value& right,
                                                      IREmitter& emitter,
                                                      FunctionCodegenContext& context) const {
    const std::string leftLength = emitter.freshTemp();
    emitter.line(leftLength + " = call i64 @strlen(ptr " + left.text + ")");
    const std::string rightLength = emitter.freshTemp();
    emitter.line(rightLength + " = call i64 @strlen(ptr " + right.text + ")");
    const std::string sumLength = emitter.freshTemp();
    emitter.line(sumLength + " = add i64 " + leftLength + ", " + rightLength);
    const std::string size = emitter.freshTemp();
    emitter.line(size + " = add i64 " + sumLength + ", 1");
    const std::string buffer = emitCheckedMalloc(size, emitter, context);
    emitter.line("call ptr @strcpy(ptr " + buffer + ", ptr " + left.text + ")");
    emitter.line("call ptr @strcat(ptr " + buffer + ", ptr " + right.text + ")");
    return Value{buffer, Type::str()};
  }

  LLVMGenerator::Impl::Value
  LLVMGenerator::Impl::generateComparisonExpression(const ast::BinaryExpression& binary,
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

  LLVMGenerator::Impl::Value
  LLVMGenerator::Impl::generateNumericBinaryExpression(const ast::BinaryExpression& binary,
                                                       const Value& left, const Value& right,
                                                       IREmitter& emitter) const {
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

    emitter.line(result + " = " + std::string(info->LLVMIntegerInstruction) + " i32 " + left.text +
                 ", " + right.text);
    return Value{result, Type::i32()};
  }

} // namespace noria
