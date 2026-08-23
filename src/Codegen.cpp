#include "noria/Codegen.hpp"

#include "noria/Builtins.hpp"
#include "noria/Diagnostic.hpp"
#include "noria/Runtime.hpp"

#include <sstream>
#include <string_view>
#include <unordered_map>

namespace noria {

  namespace {
    bool isComparison(ast::BinaryOperator op);
    std::string llvmIntegerInstruction(ast::BinaryOperator op);
    std::string llvmFloatInstruction(ast::BinaryOperator op);
    std::string llvmIntegerComparisonPredicate(ast::BinaryOperator op);
    std::string llvmFloatComparisonPredicate(ast::BinaryOperator op);

    std::string escapeForLlvmString(std::string_view value) {
      std::string escaped;
      for (const char character : value) {
        switch (character) {
        case '\\':
          escaped += "\\5C";
          break;
        case '"':
          escaped += "\\22";
          break;
        case '\n':
          escaped += "\\0A";
          break;
        case '\t':
          escaped += "\\09";
          break;
        default:
          escaped.push_back(character);
          break;
        }
      }
      return escaped;
    }
  } // namespace

  // main flow
  // gen() -> genFunction() + push scope (pop when done)-> genStatement() -> push scope if needed ->
  // genStatement() ->.... ->pop scope
  //                                                                      ->  genExpression() ->....
  std::string LlvmIrTextGenerator::generate(const ast::Module& module) const {
    CodegenContext context;
    context.functions = collectFunctionBindings(module);

    std::ostringstream functions;
    for (const auto& function : module.functions) {
      functions << generateFunction(function, context) << "\n";
    }

    return modulePreamble() + context.globals.str() + functions.str();
  }

  std::string LlvmIrTextGenerator::modulePreamble() const {
    std::string preamble;

    const std::string triple = runtime::targetTriple();
    if (!triple.empty()) {
      preamble += "target triple = \"" + triple + "\"\n";
      preamble += "target datalayout = \"" + runtime::targetDataLayout() + "\"\n";
    }

    for (const std::string_view declaration : runtime::runtimeDeclarations)
      preamble += declaration;

    for (const std::string_view global : runtime::runtimeGlobals)
      preamble += global;

    preamble += "\n";
    preamble += runtime::runtimeDefinitions;
    return preamble;
  }

  std::string LlvmIrTextGenerator::defaultIrValue(const Type& type) const {
    if (type == Type::boolean())
      return "false";
    if (type == Type::f64())
      return "0.0";
    return "0";
  }

  std::string LlvmIrTextGenerator::generateFunction(const ast::Function& function,
                                                    CodegenContext& context) const {
    const Type returnType = function.returnType;

    std::ostringstream out;
    IrEmitter emitter(out);
    out << "define " << llvmType(returnType) << " @" << function.name << "(";
    for (std::size_t index{}; index < function.parameters.size(); ++index) {
      const auto& parameter = function.parameters[index];
      const Type parameterType = parameter.type;

      if (index != 0)
        out << ", ";

      out << llvmType(parameterType) << " %" << parameter.name << ".param";
    }
    out << ") {\n";
    out << "entry:\n";

    std::vector<Scope> scopes;
    scopes.emplace_back(); // scope is an unordered_map, create an empty scope

    for (const auto& parameter : function.parameters) {
      const Type parameterType = parameter.type;
      if (!declareLocal(scopes, parameter.name,
                        LocalBinding{"%" + parameter.name, parameterType})) {
        throw CompileError("codegen: duplicate parameter '" + parameter.name + "'");
      }

      const std::string slot = "%" + parameter.name;
      emitter.emitAlloca(parameterType, slot);
      emitter.emitStore(parameterType, "%" + parameter.name + ".param", slot);
    }

    const bool emittedReturn =
        generateStatements(function.body, emitter, context, returnType, scopes);

    if (!emittedReturn) {
      out << "  ret " << llvmType(returnType) << " " << defaultIrValue(returnType) << "\n";
    }

    out << "}\n";
    return out.str();
  }

  bool LlvmIrTextGenerator::generateStatements(
      const std::vector<std::unique_ptr<ast::Statement>>& statements, IrEmitter& emitter,
      CodegenContext& context, Type expectedReturnType, std::vector<Scope>& scopes) const {

    for (const auto& statement : statements) {
      if (generateStatement(*statement, emitter, context, expectedReturnType, scopes))
        return true;
    }

    return false;
  }

  LlvmIrTextGenerator::StatementVisitor::StatementVisitor(const LlvmIrTextGenerator& generator,
                                                          IrEmitter& emitter,
                                                          CodegenContext& context,
                                                          Type expectedReturnType,
                                                          std::vector<Scope>& scopes)
      : generator_(generator), emitter_(emitter), context_(context),
        expectedReturnType_(expectedReturnType), scopes_(scopes) {}

  void LlvmIrTextGenerator::StatementVisitor::visit(const ast::LetStatement& letStatement) {
    const Type localType = letStatement.type;
    const std::string slot =
        "%" + letStatement.name + ".slot" + std::to_string(emitter_.freshTempCounter());

    if (!generator_.declareLocal(scopes_, letStatement.name, LocalBinding{slot, localType})) {
      throw CompileError("codegen: duplicate local variable '" + letStatement.name + "'");
    }

    emitter_.emitAlloca(localType, slot);

    Value initializer =
        generator_.generateRvalue(*letStatement.initializer, emitter_, context_, scopes_);
    emitter_.emitStore(localType, initializer.text, slot);
    returned_ = false;
  }

  void LlvmIrTextGenerator::StatementVisitor::visit(const ast::ReturnStatement& returnStatement) {
    Value returnValue =
        generator_.generateRvalue(*returnStatement.expression, emitter_, context_, scopes_);
    emitter_.line("ret " + llvmType(expectedReturnType_) + " " + returnValue.text);
    returned_ = true;
  }

  void LlvmIrTextGenerator::StatementVisitor::visit(
      const ast::AssignmentStatement& assignmentStatement) {
    const LocalBinding local = generator_.generatePlace(*assignmentStatement.lhs, scopes_);

    Value rvalue = generator_.generateRvalue(*assignmentStatement.rhs, emitter_, context_, scopes_);
    emitter_.emitStore(local.type, rvalue.text, local.slot);
    returned_ = false;
  }

  void LlvmIrTextGenerator::StatementVisitor::visit(const ast::IfStatement& ifStatement) {
    const int labelId = emitter_.freshLabelId();
    const std::string thenLabel = "if.then" + std::to_string(labelId);
    const std::string elseLabel = "if.else" + std::to_string(labelId);
    const std::string endLabel = "if.end" + std::to_string(labelId);

    const std::string condition =
        generator_.generateCondition(*ifStatement.condition, emitter_, context_, scopes_);
    emitter_.emitCondBranch(condition, thenLabel, elseLabel);

    emitter_.emitLabel(thenLabel);
    scopes_.emplace_back();
    const bool thenReturns = generator_.generateStatements(ifStatement.thenBranch, emitter_,
                                                           context_, expectedReturnType_, scopes_);
    scopes_.pop_back();

    if (!thenReturns)
      emitter_.emitBranch(endLabel);

    emitter_.emitLabel(elseLabel);
    scopes_.emplace_back();
    const bool elseReturns = generator_.generateStatements(ifStatement.elseBranch, emitter_,
                                                           context_, expectedReturnType_, scopes_);
    scopes_.pop_back();
    if (!elseReturns)
      emitter_.emitBranch(endLabel);

    if (!thenReturns || !elseReturns) {
      emitter_.emitLabel(endLabel);
      returned_ = false;
      return;
    }

    returned_ = true;
  }

  void LlvmIrTextGenerator::StatementVisitor::visit(const ast::WhileStatement& whileStatement) {
    const int labelId = emitter_.freshLabelId();
    const std::string conditionLabel = "while.cond" + std::to_string(labelId);
    const std::string bodyLabel = "while.body" + std::to_string(labelId);
    const std::string endLabel = "while.end" + std::to_string(labelId);

    emitter_.emitBranch(conditionLabel);

    emitter_.emitLabel(conditionLabel);
    const std::string condition =
        generator_.generateCondition(*whileStatement.condition, emitter_, context_, scopes_);
    emitter_.emitCondBranch(condition, bodyLabel, endLabel);

    emitter_.emitLabel(bodyLabel);
    scopes_.emplace_back();
    const bool bodyReturns = generator_.generateStatements(whileStatement.body, emitter_, context_,
                                                           expectedReturnType_, scopes_);
    scopes_.pop_back();
    if (!bodyReturns)
      emitter_.emitBranch(conditionLabel);

    emitter_.emitLabel(endLabel);
    returned_ = false;
  }

  void LlvmIrTextGenerator::StatementVisitor::visit(
      const ast::ExpressionStatement& expressionStatement) {
    generator_.generateRvalue(*expressionStatement.expression, emitter_, context_, scopes_);
    returned_ = false;
  }

  void LlvmIrTextGenerator::StatementVisitor::visit(const ast::IntegerLiteral&) {
    throw CompileError("codegen: internal error: expression visited by statement visitor");
  }
  void LlvmIrTextGenerator::StatementVisitor::visit(const ast::FloatLiteral&) {
    throw CompileError("codegen: internal error: expression visited by statement visitor");
  }
  void LlvmIrTextGenerator::StatementVisitor::visit(const ast::StringLiteral&) {
    throw CompileError("codegen: internal error: expression visited by statement visitor");
  }
  void LlvmIrTextGenerator::StatementVisitor::visit(const ast::BoolLiteral&) {
    throw CompileError("codegen: internal error: expression visited by statement visitor");
  }
  void LlvmIrTextGenerator::StatementVisitor::visit(const ast::UnaryExpression&) {
    throw CompileError("codegen: internal error: expression visited by statement visitor");
  }
  void LlvmIrTextGenerator::StatementVisitor::visit(const ast::CastExpression&) {
    throw CompileError("codegen: internal error: expression visited by statement visitor");
  }
  void LlvmIrTextGenerator::StatementVisitor::visit(const ast::BinaryExpression&) {
    throw CompileError("codegen: internal error: expression visited by statement visitor");
  }
  void LlvmIrTextGenerator::StatementVisitor::visit(const ast::IdentifierExpression&) {
    throw CompileError("codegen: internal error: expression visited by statement visitor");
  }
  void LlvmIrTextGenerator::StatementVisitor::visit(const ast::CallExpression&) {
    throw CompileError("codegen: internal error: expression visited by statement visitor");
  }

  LlvmIrTextGenerator::ExpressionVisitor::ExpressionVisitor(const LlvmIrTextGenerator& generator,
                                                            IrEmitter& emitter,
                                                            CodegenContext& context,
                                                            const std::vector<Scope>& scopes)
      : generator_(generator), emitter_(emitter), context_(context), scopes_(scopes) {}

  void LlvmIrTextGenerator::ExpressionVisitor::visit(const ast::IntegerLiteral& integer) {
    result_ = Value{std::to_string(integer.value), Type::i32()};
  }

  void LlvmIrTextGenerator::ExpressionVisitor::visit(const ast::FloatLiteral& floating) {
    std::ostringstream literal;
    literal << floating.value;
    std::string text = literal.str();
    if (text.find('.') == std::string::npos && text.find('e') == std::string::npos &&
        text.find('E') == std::string::npos) {
      text += ".0";
    }
    result_ = Value{text, Type::f64()};
  }

  void LlvmIrTextGenerator::ExpressionVisitor::visit(const ast::StringLiteral& stringLiteral) {
    result_ = generator_.generateStringLiteral(stringLiteral, emitter_, context_);
  }

  void LlvmIrTextGenerator::ExpressionVisitor::visit(const ast::BoolLiteral& boolean) {
    result_ = Value{boolean.value ? "true" : "false", Type::boolean()};
  }

  void LlvmIrTextGenerator::ExpressionVisitor::visit(const ast::UnaryExpression& unary) {
    const Value operand = generator_.generateRvalue(*unary.operand, emitter_, context_, scopes_);
    const std::string result = emitter_.freshTemp();

    switch (unary.op) {
    case ast::UnaryOperator::Negate:
      if (operand.type == Type::f64()) {
        emitter_.line(result + " = fneg double " + operand.text);
        result_ = Value{result, Type::f64()};
        return;
      }
      emitter_.line(result + " = sub i32 0, " + operand.text);
      result_ = Value{result, Type::i32()};
      return;
    case ast::UnaryOperator::Not:
      emitter_.line(result + " = xor i1 " + operand.text + ", true");
      result_ = Value{result, Type::boolean()};
      return;
    case ast::UnaryOperator::BitNot:
      emitter_.line(result + " = xor i32 " + operand.text + ", -1");
      result_ = Value{result, Type::i32()};
      return;
    }
  }

  void LlvmIrTextGenerator::ExpressionVisitor::visit(const ast::CastExpression& castExpression) {
    result_ = generator_.generateCastExpression(castExpression, emitter_, context_, scopes_);
  }

  void LlvmIrTextGenerator::ExpressionVisitor::visit(const ast::BinaryExpression& binary) {
    result_ = generator_.generateBinaryExpression(binary, emitter_, context_, scopes_);
  }

  void LlvmIrTextGenerator::ExpressionVisitor::visit(const ast::IdentifierExpression& identifier) {
    const LocalBinding& local = generator_.lookupLocal(scopes_, identifier.name);

    const std::string result = emitter_.freshTemp();
    emitter_.emitLoad(local.type, local.slot, result);
    result_ = Value{result, local.type};
  }

  void LlvmIrTextGenerator::ExpressionVisitor::visit(const ast::CallExpression& call) {
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
        result + " = call " + llvmType(function->second.returnType) + " @" + call.callee + "(";
    for (std::size_t index{}; index < arguments.size(); ++index) {
      if (index != 0)
        callLine += ", ";

      callLine += llvmType(arguments[index].type) + " " + arguments[index].text;
    }
    callLine += ")";
    emitter_.line(callLine);
    result_ = Value{result, function->second.returnType};
  }

  void LlvmIrTextGenerator::ExpressionVisitor::visit(const ast::ReturnStatement&) {
    throw CompileError("codegen: internal error: statement visited by expression visitor");
  }
  void LlvmIrTextGenerator::ExpressionVisitor::visit(const ast::LetStatement&) {
    throw CompileError("codegen: internal error: statement visited by expression visitor");
  }
  void LlvmIrTextGenerator::ExpressionVisitor::visit(const ast::IfStatement&) {
    throw CompileError("codegen: internal error: statement visited by expression visitor");
  }
  void LlvmIrTextGenerator::ExpressionVisitor::visit(const ast::WhileStatement&) {
    throw CompileError("codegen: internal error: statement visited by expression visitor");
  }
  void LlvmIrTextGenerator::ExpressionVisitor::visit(const ast::AssignmentStatement&) {
    throw CompileError("codegen: internal error: statement visited by expression visitor");
  }
  void LlvmIrTextGenerator::ExpressionVisitor::visit(const ast::ExpressionStatement&) {
    throw CompileError("codegen: internal error: statement visited by expression visitor");
  }

  void LlvmIrTextGenerator::ComparisonProbe::visit(const ast::BinaryExpression& node) {
    if (isComparison(node.op))
      comparison_ = &node;
  }

  void LlvmIrTextGenerator::ComparisonProbe::visit(const ast::IntegerLiteral&) {}
  void LlvmIrTextGenerator::ComparisonProbe::visit(const ast::FloatLiteral&) {}
  void LlvmIrTextGenerator::ComparisonProbe::visit(const ast::StringLiteral&) {}
  void LlvmIrTextGenerator::ComparisonProbe::visit(const ast::BoolLiteral&) {}
  void LlvmIrTextGenerator::ComparisonProbe::visit(const ast::UnaryExpression&) {}
  void LlvmIrTextGenerator::ComparisonProbe::visit(const ast::CastExpression&) {}
  void LlvmIrTextGenerator::ComparisonProbe::visit(const ast::IdentifierExpression&) {}
  void LlvmIrTextGenerator::ComparisonProbe::visit(const ast::CallExpression&) {}

  void LlvmIrTextGenerator::ComparisonProbe::visit(const ast::ReturnStatement&) {}
  void LlvmIrTextGenerator::ComparisonProbe::visit(const ast::LetStatement&) {}
  void LlvmIrTextGenerator::ComparisonProbe::visit(const ast::IfStatement&) {}
  void LlvmIrTextGenerator::ComparisonProbe::visit(const ast::WhileStatement&) {}
  void LlvmIrTextGenerator::ComparisonProbe::visit(const ast::AssignmentStatement&) {}
  void LlvmIrTextGenerator::ComparisonProbe::visit(const ast::ExpressionStatement&) {}

  LlvmIrTextGenerator::PlaceVisitor::PlaceVisitor(const LlvmIrTextGenerator& generator,
                                                  const std::vector<Scope>& scopes)
      : generator_(generator), scopes_(scopes) {}

  void LlvmIrTextGenerator::PlaceVisitor::visit(const ast::IdentifierExpression& identifier) {
    result_ = generator_.lookupLocal(scopes_, identifier.name);
  }

  void LlvmIrTextGenerator::PlaceVisitor::visit(const ast::IntegerLiteral&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LlvmIrTextGenerator::PlaceVisitor::visit(const ast::FloatLiteral&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LlvmIrTextGenerator::PlaceVisitor::visit(const ast::StringLiteral&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LlvmIrTextGenerator::PlaceVisitor::visit(const ast::BoolLiteral&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LlvmIrTextGenerator::PlaceVisitor::visit(const ast::UnaryExpression&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LlvmIrTextGenerator::PlaceVisitor::visit(const ast::CastExpression&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LlvmIrTextGenerator::PlaceVisitor::visit(const ast::BinaryExpression&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LlvmIrTextGenerator::PlaceVisitor::visit(const ast::CallExpression&) {
    throw CompileError("codegen: invalid assignment target");
  }

  void LlvmIrTextGenerator::PlaceVisitor::visit(const ast::ReturnStatement&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LlvmIrTextGenerator::PlaceVisitor::visit(const ast::LetStatement&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LlvmIrTextGenerator::PlaceVisitor::visit(const ast::IfStatement&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LlvmIrTextGenerator::PlaceVisitor::visit(const ast::WhileStatement&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LlvmIrTextGenerator::PlaceVisitor::visit(const ast::AssignmentStatement&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LlvmIrTextGenerator::PlaceVisitor::visit(const ast::ExpressionStatement&) {
    throw CompileError("codegen: invalid assignment target");
  }

  LlvmIrTextGenerator::LocalBinding
  LlvmIrTextGenerator::generatePlace(const ast::Expression& place,
                                     const std::vector<Scope>& scopes) const {
    PlaceVisitor visitor(*this, scopes);
    place.accept(visitor);
    return visitor.result();
  }

  bool LlvmIrTextGenerator::generateStatement(const ast::Statement& statement, IrEmitter& emitter,
                                              CodegenContext& context, Type expectedReturnType,
                                              std::vector<Scope>& scopes) const {
    StatementVisitor visitor(*this, emitter, context, expectedReturnType, scopes);
    statement.accept(visitor);
    return visitor.returned();
  }

  LlvmIrTextGenerator::Value
  LlvmIrTextGenerator::generateStringLiteral(const ast::StringLiteral& literal, IrEmitter& emitter,
                                             CodegenContext& context) const {
    const std::string globalName = "@.str." + std::to_string(context.nextStringGlobal++);
    const std::size_t length = literal.value.size() + 1;
    context.globals << globalName << " = private unnamed_addr constant [" << length << " x i8] c\""
                    << escapeForLlvmString(literal.value) << "\\00\"\n";

    const std::string result = emitter.freshTemp();
    emitter.line(result + " = getelementptr inbounds [" + std::to_string(length) + " x i8], ptr " +
                 globalName + ", i32 0, i32 0");
    return Value{result, Type::str()};
  }

  LlvmIrTextGenerator::Value
  LlvmIrTextGenerator::generateCastExpression(const ast::CastExpression& cast, IrEmitter& emitter,
                                              CodegenContext& context,
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

  std::optional<LlvmIrTextGenerator::Value>
  LlvmIrTextGenerator::tryGenerateBuiltinCall(const ast::CallExpression& call, IrEmitter& emitter,
                                              CodegenContext& context,
                                              const std::vector<Scope>& scopes) const {

    const BuiltinSignature* descriptor = lookupBuiltin(call.callee);
    if (descriptor == nullptr)
      return std::nullopt;

    switch (descriptor->id) {
    case BuiltinId::Println:
      emitter.line("call i32 @putchar(i32 10)");
      return Value{"", Type::voidType()};

    case BuiltinId::Print: {
      const Value argument = generateRvalue(*call.arguments[0], emitter, context, scopes);
      emitter.line("call i32 @puts(ptr " + argument.text + ")");
      return Value{"", Type::voidType()};
    }

    case BuiltinId::PrintInt: {
      const Value argument = generateRvalue(*call.arguments[0], emitter, context, scopes);
      emitter.line("call void @noria_print_int(i32 " + argument.text + ")");
      return Value{"", Type::voidType()};
    }

    case BuiltinId::PrintFloat: {
      const Value argument = generateRvalue(*call.arguments[0], emitter, context, scopes);
      const std::string formatPointer = emitter.freshTemp();
      emitter.line(formatPointer +
                   " = getelementptr inbounds [4 x i8], ptr @.fmt.float, i32 0, i32 0");
      emitter.line("call i32 @printf(ptr " + formatPointer + ", double " + argument.text + ")");
      return Value{"", Type::voidType()};
    }

    case BuiltinId::PrintChar: {
      const Value argument = generateRvalue(*call.arguments[0], emitter, context, scopes);
      emitter.line("call i32 @putchar(i32 " + argument.text + ")");
      return Value{"", Type::voidType()};
    }

    case BuiltinId::Sqrt: {
      const Value argument = generateRvalue(*call.arguments[0], emitter, context, scopes);
      const std::string result = emitter.freshTemp();
      emitter.line(result + " = call double @llvm.sqrt.f64(double " + argument.text + ")");
      return Value{result, Type::f64()};
    }

    case BuiltinId::Pow: {
      const Value base = generateRvalue(*call.arguments[0], emitter, context, scopes);
      const Value exponent = generateRvalue(*call.arguments[1], emitter, context, scopes);
      const std::string result = emitter.freshTemp();
      emitter.line(result + " = call double @llvm.pow.f64(double " + base.text + ", double " +
                   exponent.text + ")");
      return Value{result, Type::f64()};
    }
    }

    return std::nullopt;
  }

  std::string LlvmIrTextGenerator::generateCondition(const ast::Expression& expression,
                                                     IrEmitter& emitter, CodegenContext& context,
                                                     const std::vector<Scope>& scopes) const {

    ComparisonProbe probe;
    expression.accept(probe);
    if (const ast::BinaryExpression* binary = probe.comparison())
      return generateBinaryExpression(*binary, emitter, context, scopes).text;

    const Value value = generateRvalue(expression, emitter, context, scopes);

    if (value.type == Type::boolean())
      return value.text;

    const std::string result = emitter.freshTemp();
    emitter.line(result + " = icmp ne i32 " + value.text + ", 0");
    return result;
  }

  LlvmIrTextGenerator::Value
  LlvmIrTextGenerator::generateRvalue(const ast::Expression& expression, IrEmitter& emitter,
                                      CodegenContext& context,
                                      const std::vector<Scope>& scopes) const {
    ExpressionVisitor visitor(*this, emitter, context, scopes);
    expression.accept(visitor);
    return visitor.result();
  }

  LlvmIrTextGenerator::Value
  LlvmIrTextGenerator::generateBinaryExpression(const ast::BinaryExpression& binary,
                                                IrEmitter& emitter, CodegenContext& context,
                                                const std::vector<Scope>& scopes) const {

    if (binary.op == ast::BinaryOperator::And || binary.op == ast::BinaryOperator::Or) {
      const Value left = generateRvalue(*binary.left, emitter, context, scopes);
      const int labelId = emitter.freshLabelId();
      const std::string shortCircuitLabel =
          (binary.op == ast::BinaryOperator::And ? "and.short" : "or.short") +
          std::to_string(labelId);
      const std::string rhsLabel =
          (binary.op == ast::BinaryOperator::And ? "and.rhs" : "or.rhs") + std::to_string(labelId);
      const std::string mergeLabel =
          (binary.op == ast::BinaryOperator::And ? "and.end" : "or.end") + std::to_string(labelId);
      const std::string shortCircuitValue =
          binary.op == ast::BinaryOperator::And ? "false" : "true";

      if (binary.op == ast::BinaryOperator::And)
        emitter.emitCondBranch(left.text, rhsLabel, shortCircuitLabel);
      else
        emitter.emitCondBranch(left.text, shortCircuitLabel, rhsLabel);

      emitter.emitLabel(shortCircuitLabel);
      emitter.emitBranch(mergeLabel);

      emitter.emitLabel(rhsLabel);
      const Value right = generateRvalue(*binary.right, emitter, context, scopes);
      emitter.emitBranch(mergeLabel);

      emitter.emitLabel(mergeLabel);
      const std::string result = emitter.freshTemp();
      emitter.line(result + " = phi i1 [ " + shortCircuitValue + ", %" + shortCircuitLabel +
                   " ], [ " + right.text + ", %" + rhsLabel + " ]");
      return Value{result, Type::boolean()};
    }

    const Value left = generateRvalue(*binary.left, emitter, context, scopes);
    const Value right = generateRvalue(*binary.right, emitter, context, scopes);
    const std::string result = emitter.freshTemp();

    if (isComparison(binary.op)) {
      if (left.type == Type::f64() && right.type == Type::f64()) {
        emitter.line(result + " = fcmp " + llvmFloatComparisonPredicate(binary.op) + " double " +
                     left.text + ", " + right.text);
        return Value{result, Type::boolean()};
      }

      emitter.line(result + " = icmp " + llvmIntegerComparisonPredicate(binary.op) + " i32 " +
                   left.text + ", " + right.text);
      return Value{result, Type::boolean()};
    }

    if (left.type == Type::f64() && right.type == Type::f64()) {
      emitter.line(result + " = " + llvmFloatInstruction(binary.op) + " double " + left.text +
                   ", " + right.text);
      return Value{result, Type::f64()};
    }

    emitter.line(result + " = " + llvmIntegerInstruction(binary.op) + " i32 " + left.text + ", " +
                 right.text);
    return Value{result, Type::i32()};
  }

  bool LlvmIrTextGenerator::declareLocal(std::vector<Scope>& scopes, const std::string& name,
                                         LocalBinding binding) const {
    if (scopes.empty())
      scopes.emplace_back();

    auto& scope = scopes.back();
    if (scope.contains(name))
      return false;

    scope.emplace(name, std::move(binding));
    return true;
  }

  const LlvmIrTextGenerator::LocalBinding&
  LlvmIrTextGenerator::lookupLocal(const std::vector<Scope>& scopes,
                                   const std::string& name) const {

    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
      const auto local = scope->find(name);
      if (local != scope->end())
        return local->second;
    }

    throw CompileError("codegen: unknown local variable '" + name + "'");
  }

  std::unordered_map<std::string, LlvmIrTextGenerator::FunctionBinding>
  LlvmIrTextGenerator::collectFunctionBindings(const ast::Module& module) const {
    std::unordered_map<std::string, FunctionBinding> functions;

    for (const auto& function : module.functions) {
      FunctionBinding binding;
      binding.returnType = function.returnType;
      for (const auto& parameter : function.parameters) {
        binding.parameterTypes.push_back(parameter.type);
      }
      functions.emplace(function.name, std::move(binding));
    }

    return functions;
  }

  namespace {

    bool isComparison(ast::BinaryOperator op) {
      switch (op) {
      case ast::BinaryOperator::Less:
      case ast::BinaryOperator::LessEqual:
      case ast::BinaryOperator::Greater:
      case ast::BinaryOperator::GreaterEqual:
      case ast::BinaryOperator::Equal:
      case ast::BinaryOperator::NotEqual:
        return true;
      case ast::BinaryOperator::Add:
      case ast::BinaryOperator::Subtract:
      case ast::BinaryOperator::Multiply:
      case ast::BinaryOperator::Divide:
      case ast::BinaryOperator::Modulo:
      case ast::BinaryOperator::And:
      case ast::BinaryOperator::Or:
      case ast::BinaryOperator::BitAnd:
      case ast::BinaryOperator::BitOr:
      case ast::BinaryOperator::BitXor:
      case ast::BinaryOperator::Shl:
      case ast::BinaryOperator::Shr:
        return false;
      }

      return false;
    }

    std::string llvmIntegerInstruction(ast::BinaryOperator op) {
      switch (op) {
      case ast::BinaryOperator::Add:
        return "add";
      case ast::BinaryOperator::Subtract:
        return "sub";
      case ast::BinaryOperator::Multiply:
        return "mul";
      case ast::BinaryOperator::Divide:
        return "sdiv";
      case ast::BinaryOperator::Modulo:
        return "srem";
      case ast::BinaryOperator::BitAnd:
        return "and";
      case ast::BinaryOperator::BitOr:
        return "or";
      case ast::BinaryOperator::BitXor:
        return "xor";
      case ast::BinaryOperator::Shl:
        return "shl";
      case ast::BinaryOperator::Shr:
        return "ashr";
      case ast::BinaryOperator::Less:
      case ast::BinaryOperator::LessEqual:
      case ast::BinaryOperator::Greater:
      case ast::BinaryOperator::GreaterEqual:
      case ast::BinaryOperator::Equal:
      case ast::BinaryOperator::NotEqual:
      case ast::BinaryOperator::And:
      case ast::BinaryOperator::Or:
        break;
      }

      return "";
    }

    std::string llvmFloatInstruction(ast::BinaryOperator op) {
      switch (op) {
      case ast::BinaryOperator::Add:
        return "fadd";
      case ast::BinaryOperator::Subtract:
        return "fsub";
      case ast::BinaryOperator::Multiply:
        return "fmul";
      case ast::BinaryOperator::Divide:
        return "fdiv";
      case ast::BinaryOperator::Modulo:
      case ast::BinaryOperator::And:
      case ast::BinaryOperator::Or:
      case ast::BinaryOperator::BitAnd:
      case ast::BinaryOperator::BitOr:
      case ast::BinaryOperator::BitXor:
      case ast::BinaryOperator::Shl:
      case ast::BinaryOperator::Shr:
      case ast::BinaryOperator::Less:
      case ast::BinaryOperator::LessEqual:
      case ast::BinaryOperator::Greater:
      case ast::BinaryOperator::GreaterEqual:
      case ast::BinaryOperator::Equal:
      case ast::BinaryOperator::NotEqual:
        break;
      }

      return "";
    }

    std::string llvmIntegerComparisonPredicate(ast::BinaryOperator op) {
      switch (op) {
      case ast::BinaryOperator::Less:
        return "slt";
      case ast::BinaryOperator::LessEqual:
        return "sle";
      case ast::BinaryOperator::Greater:
        return "sgt";
      case ast::BinaryOperator::GreaterEqual:
        return "sge";
      case ast::BinaryOperator::Equal:
        return "eq";
      case ast::BinaryOperator::NotEqual:
        return "ne";
      case ast::BinaryOperator::Add:
      case ast::BinaryOperator::Subtract:
      case ast::BinaryOperator::Multiply:
      case ast::BinaryOperator::Divide:
      case ast::BinaryOperator::Modulo:
      case ast::BinaryOperator::And:
      case ast::BinaryOperator::Or:
      case ast::BinaryOperator::BitAnd:
      case ast::BinaryOperator::BitOr:
      case ast::BinaryOperator::BitXor:
      case ast::BinaryOperator::Shl:
      case ast::BinaryOperator::Shr:
        break;
      }

      return "";
    }

    std::string llvmFloatComparisonPredicate(ast::BinaryOperator op) {
      switch (op) {
      case ast::BinaryOperator::Less:
        return "olt";
      case ast::BinaryOperator::LessEqual:
        return "ole";
      case ast::BinaryOperator::Greater:
        return "ogt";
      case ast::BinaryOperator::GreaterEqual:
        return "oge";
      case ast::BinaryOperator::Equal:
        return "oeq";
      case ast::BinaryOperator::NotEqual:
        return "one";
      case ast::BinaryOperator::Add:
      case ast::BinaryOperator::Subtract:
      case ast::BinaryOperator::Multiply:
      case ast::BinaryOperator::Divide:
      case ast::BinaryOperator::Modulo:
      case ast::BinaryOperator::And:
      case ast::BinaryOperator::Or:
      case ast::BinaryOperator::BitAnd:
      case ast::BinaryOperator::BitOr:
      case ast::BinaryOperator::BitXor:
      case ast::BinaryOperator::Shl:
      case ast::BinaryOperator::Shr:
        break;
      }

      return "";
    }

  } // namespace

} // namespace noria
