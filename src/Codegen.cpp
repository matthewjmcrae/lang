#include "noria/Codegen.hpp"

#include "noria/Builtins.hpp"
#include "noria/Diagnostic.hpp"

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
    collectFunctionBindings(module);
    moduleGlobals_.str("");
    moduleGlobals_.clear();
    nextStringGlobal_ = 0;

    std::ostringstream functions;
    for (const auto& function : module.functions) {
      functions << generateFunction(function) << "\n";
    }

    return modulePreamble() + moduleGlobals_.str() + functions.str();
  }

  std::string LlvmIrTextGenerator::modulePreamble() const {
    std::string preamble;

#if defined(__APPLE__) && defined(__aarch64__)
    preamble += "target triple = \"arm64-apple-macosx\"\n";
    preamble += "target datalayout = \"e-m:o-i64:64-i128:128-n32:64-S128\"\n";
#elif defined(__APPLE__) && defined(__x86_64__)
    preamble += "target triple = \"x86_64-apple-macosx\"\n";
    preamble += "target datalayout = \"e-m:o-i64:64-m:x86-64:32-f80:128-n8:16:32:64-S128\"\n";
#elif defined(__linux__) && defined(__aarch64__)
    preamble += "target triple = \"aarch64-unknown-linux-gnu\"\n";
    preamble += "target datalayout = \"e-m:e-i64:64-i128:128-n32:64-S128\"\n";
#elif defined(__linux__) && defined(__x86_64__)
    preamble += "target triple = \"x86_64-unknown-linux-gnu\"\n";
    preamble += "target datalayout = \"e-m:e-i64:64-f80:128-n8:16:32:64-S128\"\n";
#endif

    preamble += "declare i32 @printf(ptr, ...)\n"
                "declare i32 @puts(ptr)\n"
                "declare i32 @putchar(i32)\n"
                "declare double @llvm.sqrt.f64(double)\n"
                "declare double @llvm.pow.f64(double, double)\n"
                "@.fmt.float = private unnamed_addr constant [4 x i8] c\"%g\\0A\\00\"\n\n"
                "define void @noria_print_int(i32 %value) {\n"
                "entry:\n"
                "  %is_zero = icmp eq i32 %value, 0\n"
                "  br i1 %is_zero, label %zero, label %check_sign\n"
                "zero:\n"
                "  call i32 @putchar(i32 48)\n"
                "  call i32 @putchar(i32 10)\n"
                "  ret void\n"
                "check_sign:\n"
                "  %is_neg = icmp slt i32 %value, 0\n"
                "  br i1 %is_neg, label %negate, label %digits\n"
                "negate:\n"
                "  call i32 @putchar(i32 45)\n"
                "  %abs = sub i32 0, %value\n"
                "  br label %digits\n"
                "digits:\n"
                "  %n = phi i32 [ %value, %check_sign ], [ %abs, %negate ]\n"
                "  %v = alloca i32\n"
                "  store i32 %n, ptr %v\n"
                "  %pos = alloca i32\n"
                "  store i32 0, ptr %pos\n"
                "  %buf = alloca [12 x i8]\n"
                "  br label %extract\n"
                "extract:\n"
                "  %cur = load i32, ptr %v\n"
                "  %done = icmp eq i32 %cur, 0\n"
                "  br i1 %done, label %print, label %push\n"
                "push:\n"
                "  %digit = urem i32 %cur, 10\n"
                "  %p = load i32, ptr %pos\n"
                "  %ch = add i32 %digit, 48\n"
                "  %slot = getelementptr [12 x i8], ptr %buf, i32 0, i32 %p\n"
                "  %byte = trunc i32 %ch to i8\n"
                "  store i8 %byte, ptr %slot\n"
                "  %pnext = add i32 %p, 1\n"
                "  store i32 %pnext, ptr %pos\n"
                "  %next = udiv i32 %cur, 10\n"
                "  store i32 %next, ptr %v\n"
                "  br label %extract\n"
                "print:\n"
                "  %count = load i32, ptr %pos\n"
                "  %idx = alloca i32\n"
                "  store i32 %count, ptr %idx\n"
                "  br label %print_loop\n"
                "print_loop:\n"
                "  %i = load i32, ptr %idx\n"
                "  %more = icmp ugt i32 %i, 0\n"
                "  br i1 %more, label %print_one, label %newline\n"
                "print_one:\n"
                "  %i1 = sub i32 %i, 1\n"
                "  %cp = getelementptr [12 x i8], ptr %buf, i32 0, i32 %i1\n"
                "  %loaded = load i8, ptr %cp\n"
                "  %c = zext i8 %loaded to i32\n"
                "  call i32 @putchar(i32 %c)\n"
                "  store i32 %i1, ptr %idx\n"
                "  br label %print_loop\n"
                "newline:\n"
                "  call i32 @putchar(i32 10)\n"
                "  ret void\n"
                "}\n\n";
    return preamble;
  }

  std::string LlvmIrTextGenerator::defaultIrValue(const Type& type) const {
    if (type == Type::boolean())
      return "false";
    if (type == Type::f64())
      return "0.0";
    return "0";
  }

  std::string LlvmIrTextGenerator::generateFunction(const ast::Function& function) const {
    const Type returnType = function.returnType;

    std::ostringstream out;
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

    int nextTemporary = 0; // unique LLVM register names
    int nextLabel = 0;     // unique LLVM label names
    std::vector<Scope> scopes;
    scopes.emplace_back(); // scope is an unordered_map, create an empty scope

    for (const auto& parameter : function.parameters) {
      const Type parameterType = parameter.type;
      if (!declareLocal(scopes, parameter.name,
                        LocalBinding{"%" + parameter.name, parameterType})) {
        throw CompileError("codegen: duplicate parameter '" + parameter.name + "'");
      }

      // allocate parameter on the stack
      const std::string slot = "%" + parameter.name;
      out << "  " << slot << " = alloca " << llvmType(parameterType) << "\n";
      out << "  store " << llvmType(parameterType) << " %" << parameter.name << ".param, ptr "
          << slot << "\n";
    }

    const bool emittedReturn =
        generateStatements(function.body, out, nextTemporary, nextLabel, returnType, scopes);

    if (!emittedReturn) {
      out << "  ret " << llvmType(returnType) << " " << defaultIrValue(returnType) << "\n";
    }

    out << "}\n";
    return out.str();
  }

  bool LlvmIrTextGenerator::generateStatements(
      const std::vector<std::unique_ptr<ast::Statement>>& statements, std::ostringstream& out,
      int& nextTemporary, int& nextLabel, Type expectedReturnType,
      std::vector<Scope>& scopes) const {

    for (const auto& statement : statements) {
      if (generateStatement(*statement, out, nextTemporary, nextLabel, expectedReturnType, scopes))
        return true;
    }

    return false;
  }

  LlvmIrTextGenerator::StatementVisitor::StatementVisitor(const LlvmIrTextGenerator& generator,
                                                          std::ostringstream& out,
                                                          int& nextTemporary, int& nextLabel,
                                                          Type expectedReturnType,
                                                          std::vector<Scope>& scopes)
      : generator_(generator), out_(out), nextTemporary_(nextTemporary), nextLabel_(nextLabel),
        expectedReturnType_(expectedReturnType), scopes_(scopes) {}

  void LlvmIrTextGenerator::StatementVisitor::visit(const ast::LetStatement& letStatement) {
    const Type localType = letStatement.type;
    const std::string slot = "%" + letStatement.name + ".slot" + std::to_string(nextTemporary_++);

    if (!generator_.declareLocal(scopes_, letStatement.name, LocalBinding{slot, localType})) {
      throw CompileError("codegen: duplicate local variable '" + letStatement.name + "'");
    }

    out_ << "  " << slot << " = alloca " << llvmType(localType) << "\n";

    Value initializer = generator_.generateExpression(*letStatement.initializer, out_,
                                                      nextTemporary_, nextLabel_, scopes_);
    out_ << "  store " << llvmType(localType) << " " << initializer.text << ", ptr " << slot
         << "\n";
    returned_ = false;
  }

  void LlvmIrTextGenerator::StatementVisitor::visit(const ast::ReturnStatement& returnStatement) {
    Value returnValue = generator_.generateExpression(*returnStatement.expression, out_,
                                                      nextTemporary_, nextLabel_, scopes_);
    out_ << "  ret " << llvmType(expectedReturnType_) << " " << returnValue.text << "\n";
    returned_ = true;
  }

  void LlvmIrTextGenerator::StatementVisitor::visit(
      const ast::AssignmentStatement& assignmentStatement) {
    const LocalBinding& local = generator_.lookupLocal(scopes_, assignmentStatement.lhs);

    Value rvalue = generator_.generateExpression(*assignmentStatement.rhs, out_, nextTemporary_,
                                                 nextLabel_, scopes_);
    out_ << "  store " << llvmType(local.type) << " " << rvalue.text << ", ptr " << local.slot
         << "\n";
    returned_ = false;
  }

  void LlvmIrTextGenerator::StatementVisitor::visit(const ast::IfStatement& ifStatement) {
    const int labelId = nextLabel_++;
    const std::string thenLabel = "if.then" + std::to_string(labelId);
    const std::string elseLabel = "if.else" + std::to_string(labelId);
    const std::string endLabel = "if.end" + std::to_string(labelId);

    const std::string condition = generator_.generateCondition(*ifStatement.condition, out_,
                                                               nextTemporary_, nextLabel_, scopes_);
    out_ << "  br i1 " << condition << ", label %" << thenLabel << ", label %" << elseLabel << "\n";

    out_ << thenLabel << ":\n";
    scopes_.emplace_back();
    const bool thenReturns = generator_.generateStatements(
        ifStatement.thenBranch, out_, nextTemporary_, nextLabel_, expectedReturnType_, scopes_);
    scopes_.pop_back();

    if (!thenReturns)
      out_ << "  br label %" << endLabel << "\n";

    out_ << elseLabel << ":\n";
    scopes_.emplace_back();
    const bool elseReturns = generator_.generateStatements(
        ifStatement.elseBranch, out_, nextTemporary_, nextLabel_, expectedReturnType_, scopes_);
    scopes_.pop_back();
    if (!elseReturns)
      out_ << "  br label %" << endLabel << "\n";

    if (!thenReturns || !elseReturns) {
      out_ << endLabel << ":\n";
      returned_ = false;
      return;
    }

    returned_ = true;
  }

  void LlvmIrTextGenerator::StatementVisitor::visit(const ast::WhileStatement& whileStatement) {
    const int labelId = nextLabel_++;
    const std::string conditionLabel = "while.cond" + std::to_string(labelId);
    const std::string bodyLabel = "while.body" + std::to_string(labelId);
    const std::string endLabel = "while.end" + std::to_string(labelId);

    out_ << "  br label %" << conditionLabel << "\n";

    out_ << conditionLabel << ":\n";
    const std::string condition = generator_.generateCondition(*whileStatement.condition, out_,
                                                               nextTemporary_, nextLabel_, scopes_);
    out_ << "  br i1 " << condition << ", label %" << bodyLabel << ", label %" << endLabel << "\n";

    out_ << bodyLabel << ":\n";
    scopes_.emplace_back();
    const bool bodyReturns = generator_.generateStatements(
        whileStatement.body, out_, nextTemporary_, nextLabel_, expectedReturnType_, scopes_);
    scopes_.pop_back();
    if (!bodyReturns)
      out_ << "  br label %" << conditionLabel << "\n";

    out_ << endLabel << ":\n";
    returned_ = false;
  }

  void LlvmIrTextGenerator::StatementVisitor::visit(
      const ast::ExpressionStatement& expressionStatement) {
    generator_.generateExpression(*expressionStatement.expression, out_, nextTemporary_, nextLabel_,
                                  scopes_);
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
                                                            std::ostringstream& out,
                                                            int& nextTemporary, int& nextLabel,
                                                            const std::vector<Scope>& scopes)
      : generator_(generator), out_(out), nextTemporary_(nextTemporary), nextLabel_(nextLabel),
        scopes_(scopes) {}

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
    result_ = generator_.generateStringLiteral(stringLiteral, out_, nextTemporary_);
  }

  void LlvmIrTextGenerator::ExpressionVisitor::visit(const ast::BoolLiteral& boolean) {
    result_ = Value{boolean.value ? "true" : "false", Type::boolean()};
  }

  void LlvmIrTextGenerator::ExpressionVisitor::visit(const ast::UnaryExpression& unary) {
    const Value operand =
        generator_.generateExpression(*unary.operand, out_, nextTemporary_, nextLabel_, scopes_);
    const std::string result = "%t" + std::to_string(nextTemporary_++);

    switch (unary.op) {
    case ast::UnaryOperator::Negate:
      if (operand.type == Type::f64()) {
        out_ << "  " << result << " = fneg double " << operand.text << "\n";
        result_ = Value{result, Type::f64()};
        return;
      }
      out_ << "  " << result << " = sub i32 0, " << operand.text << "\n";
      result_ = Value{result, Type::i32()};
      return;
    case ast::UnaryOperator::Not:
      out_ << "  " << result << " = xor i1 " << operand.text << ", true\n";
      result_ = Value{result, Type::boolean()};
      return;
    case ast::UnaryOperator::BitNot:
      out_ << "  " << result << " = xor i32 " << operand.text << ", -1\n";
      result_ = Value{result, Type::i32()};
      return;
    }
  }

  void LlvmIrTextGenerator::ExpressionVisitor::visit(const ast::CastExpression& castExpression) {
    result_ = generator_.generateCastExpression(castExpression, out_, nextTemporary_, nextLabel_,
                                                scopes_);
  }

  void LlvmIrTextGenerator::ExpressionVisitor::visit(const ast::BinaryExpression& binary) {
    result_ =
        generator_.generateBinaryExpression(binary, out_, nextTemporary_, nextLabel_, scopes_);
  }

  void LlvmIrTextGenerator::ExpressionVisitor::visit(const ast::IdentifierExpression& identifier) {
    const LocalBinding& local = generator_.lookupLocal(scopes_, identifier.name);

    const std::string result = "%t" + std::to_string(nextTemporary_++);
    out_ << "  " << result << " = load " << llvmType(local.type) << ", ptr " << local.slot << "\n";
    result_ = Value{result, local.type};
  }

  void LlvmIrTextGenerator::ExpressionVisitor::visit(const ast::CallExpression& call) {
    if (auto builtin =
            generator_.tryGenerateBuiltinCall(call, out_, nextTemporary_, nextLabel_, scopes_)) {
      result_ = *builtin;
      return;
    }

    const auto function = generator_.functions_.find(call.callee);
    if (function == generator_.functions_.end())
      throw CompileError("codegen: unknown function '" + call.callee + "'");

    std::vector<Value> arguments;
    arguments.reserve(call.arguments.size());

    for (const auto& argument : call.arguments) {
      arguments.push_back(
          generator_.generateExpression(*argument, out_, nextTemporary_, nextLabel_, scopes_));
    }

    const std::string result = "%t" + std::to_string(nextTemporary_++);
    out_ << "  " << result << " = call " << llvmType(function->second.returnType) << " @"
         << call.callee << "(";
    for (std::size_t index{}; index < arguments.size(); ++index) {
      if (index != 0)
        out_ << ", ";

      out_ << llvmType(arguments[index].type) << " " << arguments[index].text;
    }
    out_ << ")\n";
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

  bool LlvmIrTextGenerator::generateStatement(const ast::Statement& statement,
                                              std::ostringstream& out, int& nextTemporary,
                                              int& nextLabel, Type expectedReturnType,
                                              std::vector<Scope>& scopes) const {
    StatementVisitor visitor(*this, out, nextTemporary, nextLabel, expectedReturnType, scopes);
    statement.accept(visitor);
    return visitor.returned();
  }

  LlvmIrTextGenerator::Value
  LlvmIrTextGenerator::generateStringLiteral(const ast::StringLiteral& literal,
                                             std::ostringstream& out, int& nextTemporary) const {
    const std::string globalName = "@.str." + std::to_string(nextStringGlobal_++);
    const std::size_t length = literal.value.size() + 1;
    moduleGlobals_ << globalName << " = private unnamed_addr constant [" << length << " x i8] c\""
                   << escapeForLlvmString(literal.value) << "\\00\"\n";

    const std::string result = "%t" + std::to_string(nextTemporary++);
    out << "  " << result << " = getelementptr inbounds [" << length << " x i8], ptr " << globalName
        << ", i32 0, i32 0\n";
    return Value{result, Type::str()};
  }

  LlvmIrTextGenerator::Value LlvmIrTextGenerator::generateCastExpression(
      const ast::CastExpression& cast, std::ostringstream& out, int& nextTemporary, int& nextLabel,
      const std::vector<Scope>& scopes) const {
    const Value source =
        generateExpression(*cast.expression, out, nextTemporary, nextLabel, scopes);
    const Type targetType = cast.targetType;

    if (source.type == targetType)
      return source;

    const std::string result = "%t" + std::to_string(nextTemporary++);

    if (source.type == Type::i32() && targetType == Type::f64()) {
      out << "  " << result << " = sitofp i32 " << source.text << " to double\n";
      return Value{result, Type::f64()};
    }

    if (source.type == Type::f64() && targetType == Type::i32()) {
      out << "  " << result << " = fptosi double " << source.text << " to i32\n";
      return Value{result, Type::i32()};
    }

    if (source.type == Type::boolean() && targetType == Type::i32()) {
      out << "  " << result << " = zext i1 " << source.text << " to i32\n";
      return Value{result, Type::i32()};
    }

    if (source.type == Type::i32() && targetType == Type::boolean()) {
      out << "  " << result << " = icmp ne i32 " << source.text << ", 0\n";
      return Value{result, Type::boolean()};
    }

    throw CompileError("codegen: unsupported cast");
  }

  std::optional<LlvmIrTextGenerator::Value> LlvmIrTextGenerator::tryGenerateBuiltinCall(
      const ast::CallExpression& call, std::ostringstream& out, int& nextTemporary, int& nextLabel,
      const std::vector<Scope>& scopes) const {

    const BuiltinSignature* descriptor = lookupBuiltin(call.callee);
    if (descriptor == nullptr)
      return std::nullopt;

    switch (descriptor->id) {
    case BuiltinId::Println:
      out << "  call i32 @putchar(i32 10)\n";
      return Value{"", Type::voidType()};

    case BuiltinId::Print: {
      const Value argument =
          generateExpression(*call.arguments[0], out, nextTemporary, nextLabel, scopes);
      out << "  call i32 @puts(ptr " << argument.text << ")\n";
      return Value{"", Type::voidType()};
    }

    case BuiltinId::PrintInt: {
      const Value argument =
          generateExpression(*call.arguments[0], out, nextTemporary, nextLabel, scopes);
      out << "  call void @noria_print_int(i32 " << argument.text << ")\n";
      return Value{"", Type::voidType()};
    }

    case BuiltinId::PrintFloat: {
      const Value argument =
          generateExpression(*call.arguments[0], out, nextTemporary, nextLabel, scopes);
      const std::string formatPointer = "%t" + std::to_string(nextTemporary++);
      out << "  " << formatPointer
          << " = getelementptr inbounds [4 x i8], ptr @.fmt.float, i32 0, i32 0\n";
      out << "  call i32 @printf(ptr " << formatPointer << ", double " << argument.text << ")\n";
      return Value{"", Type::voidType()};
    }

    case BuiltinId::PrintChar: {
      const Value argument =
          generateExpression(*call.arguments[0], out, nextTemporary, nextLabel, scopes);
      out << "  call i32 @putchar(i32 " << argument.text << ")\n";
      return Value{"", Type::voidType()};
    }

    case BuiltinId::Sqrt: {
      const Value argument =
          generateExpression(*call.arguments[0], out, nextTemporary, nextLabel, scopes);
      const std::string result = "%t" + std::to_string(nextTemporary++);
      out << "  " << result << " = call double @llvm.sqrt.f64(double " << argument.text << ")\n";
      return Value{result, Type::f64()};
    }

    case BuiltinId::Pow: {
      const Value base =
          generateExpression(*call.arguments[0], out, nextTemporary, nextLabel, scopes);
      const Value exponent =
          generateExpression(*call.arguments[1], out, nextTemporary, nextLabel, scopes);
      const std::string result = "%t" + std::to_string(nextTemporary++);
      out << "  " << result << " = call double @llvm.pow.f64(double " << base.text << ", double "
          << exponent.text << ")\n";
      return Value{result, Type::f64()};
    }
    }

    return std::nullopt;
  }

  std::string LlvmIrTextGenerator::generateCondition(const ast::Expression& expression,
                                                     std::ostringstream& out, int& nextTemporary,
                                                     int& nextLabel,
                                                     const std::vector<Scope>& scopes) const {

    ComparisonProbe probe;
    expression.accept(probe);
    if (const ast::BinaryExpression* binary = probe.comparison())
      return generateBinaryExpression(*binary, out, nextTemporary, nextLabel, scopes).text;

    const Value value = generateExpression(expression, out, nextTemporary, nextLabel, scopes);

    if (value.type == Type::boolean())
      return value.text;

    const std::string result = "%t" + std::to_string(nextTemporary++);
    out << "  " << result << " = icmp ne i32 " << value.text << ", 0\n";
    return result;
  }

  LlvmIrTextGenerator::Value
  LlvmIrTextGenerator::generateExpression(const ast::Expression& expression,
                                          std::ostringstream& out, int& nextTemporary,
                                          int& nextLabel, const std::vector<Scope>& scopes) const {
    ExpressionVisitor visitor(*this, out, nextTemporary, nextLabel, scopes);
    expression.accept(visitor);
    return visitor.result();
  }

  LlvmIrTextGenerator::Value LlvmIrTextGenerator::generateBinaryExpression(
      const ast::BinaryExpression& binary, std::ostringstream& out, int& nextTemporary,
      int& nextLabel, const std::vector<Scope>& scopes) const {

    if (binary.op == ast::BinaryOperator::And || binary.op == ast::BinaryOperator::Or) {
      const Value left = generateExpression(*binary.left, out, nextTemporary, nextLabel, scopes);
      const int labelId = nextLabel++;
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
        out << "  br i1 " << left.text << ", label %" << rhsLabel << ", label %"
            << shortCircuitLabel << "\n";
      else
        out << "  br i1 " << left.text << ", label %" << shortCircuitLabel << ", label %"
            << rhsLabel << "\n";

      out << shortCircuitLabel << ":\n";
      out << "  br label %" << mergeLabel << "\n";

      out << rhsLabel << ":\n";
      const Value right = generateExpression(*binary.right, out, nextTemporary, nextLabel, scopes);
      out << "  br label %" << mergeLabel << "\n";

      out << mergeLabel << ":\n";
      const std::string result = "%t" + std::to_string(nextTemporary++);
      out << "  " << result << " = phi i1 [ " << shortCircuitValue << ", %" << shortCircuitLabel
          << " ], [ " << right.text << ", %" << rhsLabel << " ]\n";
      return Value{result, Type::boolean()};
    }

    const Value left = generateExpression(*binary.left, out, nextTemporary, nextLabel, scopes);
    const Value right = generateExpression(*binary.right, out, nextTemporary, nextLabel, scopes);
    const std::string result = "%t" + std::to_string(nextTemporary++);

    if (isComparison(binary.op)) {
      if (left.type == Type::f64() && right.type == Type::f64()) {
        out << "  " << result << " = fcmp " << llvmFloatComparisonPredicate(binary.op) << " double "
            << left.text << ", " << right.text << "\n";
        return Value{result, Type::boolean()};
      }

      out << "  " << result << " = icmp " << llvmIntegerComparisonPredicate(binary.op) << " i32 "
          << left.text << ", " << right.text << "\n";
      return Value{result, Type::boolean()};
    }

    if (left.type == Type::f64() && right.type == Type::f64()) {
      out << "  " << result << " = " << llvmFloatInstruction(binary.op) << " double " << left.text
          << ", " << right.text << "\n";
      return Value{result, Type::f64()};
    }

    out << "  " << result << " = " << llvmIntegerInstruction(binary.op) << " i32 " << left.text
        << ", " << right.text << "\n";
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

  void LlvmIrTextGenerator::collectFunctionBindings(const ast::Module& module) const {
    functions_.clear();

    for (const auto& function : module.functions) {
      FunctionBinding binding;
      binding.returnType = function.returnType;
      for (const auto& parameter : function.parameters) {
        binding.parameterTypes.push_back(parameter.type);
      }
      functions_.emplace(function.name, std::move(binding));
    }
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
