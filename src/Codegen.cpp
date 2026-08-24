#include "noria/Codegen.hpp"

#include "noria/Builtins.hpp"
#include "noria/Diagnostic.hpp"
#include "noria/Runtime.hpp"
#include "noria/SemanticTables.hpp"

#include <array>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace noria {

  namespace {
    std::string escapeForLLVMString(std::string_view value) {
      std::string escaped;
      for (const unsigned char character : value) {
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
          if (character >= 32 && character <= 126) {
            escaped.push_back(static_cast<char>(character));
          } else {
            static constexpr char hex[] = "0123456789ABCDEF";
            escaped += '\\';
            escaped += hex[character >> 4];
            escaped += hex[character & 0xF];
          }
          break;
        }
      }
      return escaped;
    }

    std::size_t elementSizeInBytes(const Type& type) {
      if (type.kind == TypeKind::Struct) {
        throw CompileError("codegen: struct element size is not supported");
      }

      if (type.kind == TypeKind::ImplTag) {
        throw CompileError("codegen: internal: implementation tag is not a runtime type");
      }

      if (type.kind == TypeKind::TypeParam) {
        throw CompileError("internal: unsubstituted type parameter");
      }

      if (const TypeKindInfo* info = typeKindInfo(type.kind); info && info->runtimeElementSize) {
        return *info->runtimeElementSize;
      }

      throw CompileError("codegen: unsupported array element type");
    }

    std::optional<Type> firstNonImplTagTypeArg(const std::vector<Type>& typeArgs) {
      for (const Type& typeArg : typeArgs) {
        if (typeArg.kind != TypeKind::ImplTag) {
          return typeArg;
        }
      }
      return std::nullopt;
    }

    Type resolveWitnessType(
        const std::unordered_map<std::string, std::vector<Type>>& specializationTypeArgs,
        std::string_view currentFunctionName) {
      const auto specialization = specializationTypeArgs.find(std::string(currentFunctionName));
      if (specialization == specializationTypeArgs.end()) {
        throw CompileError("codegen: witness-polymorphic runtime builtin requires an enclosing "
                           "generic specialization context");
      }

      const std::optional<Type> witness = firstNonImplTagTypeArg(specialization->second);
      if (!witness) {
        throw CompileError("codegen: witness-polymorphic runtime builtin requires an enclosing "
                           "generic specialization context");
      }

      return *witness;
    }
  } // namespace

  // main flow
  // gen() -> genFunction() + push scope (pop when done)-> genStatement() -> push scope if needed ->
  // genStatement() ->.... ->pop scope
  //                                                                      ->  genExpression() ->....
  std::string LLVMGenerator::generate(const ast::Module& module) const {
    CodegenContext context;
    context.functions = collectFunctionBindings(module);
    context.structs = collectStructLayouts(module);

    std::ostringstream functions;
    for (const auto& function : module.functions) {
      if (!function.typeParams.empty()) {
        continue;
      }
      functions << generateFunction(function, context) << "\n";
    }

    return modulePreamble() + emitStructTypeDefinitions(module) + context.globals.str() +
           functions.str();
  }

  std::string LLVMGenerator::modulePreamble() const {
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
    const std::string_view trapDefinition = runtime::runtimeTrapDefinition();
    if (!trapDefinition.empty())
      preamble += trapDefinition;
    return preamble;
  }

  std::string LLVMGenerator::defaultIRValue(const Type& type) const {
    if (type == Type::boolean())
      return "false";
    if (type == Type::f64())
      return "0.0";
    if (type.kind == TypeKind::Struct)
      return "zeroinitializer";
    if (type.kind == TypeKind::Str || type.kind == TypeKind::Array || type.kind == TypeKind::RawPtr)
      return "null";
    return "0";
  }

  std::string LLVMGenerator::generateFunction(const ast::Function& function,
                                              CodegenContext& context) const {
    context.currentFunctionName = function.name;
    const Type returnType = function.returnType;

    std::ostringstream out;
    IREmitter emitter(out);
    out << "define " << LLVMType(returnType) << " @" << function.name << "(";
    for (std::size_t index{}; index < function.parameters.size(); ++index) {
      const auto& parameter = function.parameters[index];
      const Type parameterType = parameter.type;

      if (index != 0)
        out << ", ";

      out << LLVMType(parameterType) << " %" << parameter.name << ".param";
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
      out << "  ret " << LLVMType(returnType) << " " << defaultIRValue(returnType) << "\n";
    }

    out << "}\n";
    return out.str();
  }

  bool
  LLVMGenerator::generateStatements(const std::vector<std::unique_ptr<ast::Statement>>& statements,
                                    IREmitter& emitter, CodegenContext& context,
                                    Type expectedReturnType, std::vector<Scope>& scopes) const {

    for (const auto& statement : statements) {
      if (generateStatement(*statement, emitter, context, expectedReturnType, scopes))
        return true;
    }

    return false;
  }

  LLVMGenerator::StatementVisitor::StatementVisitor(const LLVMGenerator& generator,
                                                    IREmitter& emitter, CodegenContext& context,
                                                    Type expectedReturnType,
                                                    std::vector<Scope>& scopes)
      : generator_(generator), emitter_(emitter), context_(context),
        expectedReturnType_(expectedReturnType), scopes_(scopes) {}

  void LLVMGenerator::StatementVisitor::visit(const ast::LetStatement& letStatement) {
    std::optional<Value> initializer;
    if (letStatement.initializer) {
      initializer =
          generator_.generateRvalue(*letStatement.initializer, emitter_, context_, scopes_);
    }

    const Type localType =
        letStatement.declaredType ? *letStatement.declaredType : initializer->type;
    const std::string slot =
        "%" + letStatement.name + ".slot" + std::to_string(emitter_.freshTempCounter());

    emitter_.emitAlloca(localType, slot);

    if (!generator_.declareLocal(scopes_, letStatement.name, LocalBinding{slot, localType})) {
      throw CompileError("codegen: duplicate local variable '" + letStatement.name + "'");
    }

    if (initializer) {
      emitter_.emitStore(localType, initializer->text, slot);
    } else {
      emitter_.emitStore(localType, generator_.defaultIRValue(localType), slot);
    }
    returned_ = false;
  }

  void LLVMGenerator::StatementVisitor::visit(const ast::ReturnStatement& returnStatement) {
    Value returnValue =
        generator_.generateRvalue(*returnStatement.expression, emitter_, context_, scopes_);
    emitter_.line("ret " + LLVMType(expectedReturnType_) + " " + returnValue.text);
    returned_ = true;
  }

  void LLVMGenerator::StatementVisitor::visit(const ast::AssignmentStatement& assignmentStatement) {
    const LocalBinding local =
        generator_.generatePlace(*assignmentStatement.lhs, emitter_, context_, scopes_);

    Value rvalue = generator_.generateRvalue(*assignmentStatement.rhs, emitter_, context_, scopes_);
    if (local.byteBuffer) {
      generator_.emitBufferStore(local.type, rvalue.text, local.slot, emitter_);
    } else {
      emitter_.emitStore(local.type, rvalue.text, local.slot);
    }
    returned_ = false;
  }

  void LLVMGenerator::StatementVisitor::visit(const ast::IfStatement& ifStatement) {
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

  void LLVMGenerator::StatementVisitor::visit(const ast::WhileStatement& whileStatement) {
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

  void LLVMGenerator::StatementVisitor::visit(const ast::ExpressionStatement& expressionStatement) {
    generator_.generateRvalue(*expressionStatement.expression, emitter_, context_, scopes_);
    returned_ = false;
  }

  void LLVMGenerator::StatementVisitor::visit(const ast::IntegerLiteral&) {
    throw CompileError("codegen: internal error: expression visited by statement visitor");
  }
  void LLVMGenerator::StatementVisitor::visit(const ast::FloatLiteral&) {
    throw CompileError("codegen: internal error: expression visited by statement visitor");
  }
  void LLVMGenerator::StatementVisitor::visit(const ast::StringLiteral&) {
    throw CompileError("codegen: internal error: expression visited by statement visitor");
  }
  void LLVMGenerator::StatementVisitor::visit(const ast::BoolLiteral&) {
    throw CompileError("codegen: internal error: expression visited by statement visitor");
  }
  void LLVMGenerator::StatementVisitor::visit(const ast::UnaryExpression&) {
    throw CompileError("codegen: internal error: expression visited by statement visitor");
  }
  void LLVMGenerator::StatementVisitor::visit(const ast::CastExpression&) {
    throw CompileError("codegen: internal error: expression visited by statement visitor");
  }
  void LLVMGenerator::StatementVisitor::visit(const ast::BinaryExpression&) {
    throw CompileError("codegen: internal error: expression visited by statement visitor");
  }
  void LLVMGenerator::StatementVisitor::visit(const ast::IdentifierExpression&) {
    throw CompileError("codegen: internal error: expression visited by statement visitor");
  }
  void LLVMGenerator::StatementVisitor::visit(const ast::CallExpression&) {
    throw CompileError("codegen: internal error: expression visited by statement visitor");
  }
  void LLVMGenerator::StatementVisitor::visit(const ast::ArrayLiteral&) {
    throw CompileError("codegen: internal error: expression visited by statement visitor");
  }
  void LLVMGenerator::StatementVisitor::visit(const ast::IndexExpression&) {
    throw CompileError("codegen: internal error: expression visited by statement visitor");
  }
  void LLVMGenerator::StatementVisitor::visit(const ast::StructLiteral&) {
    throw CompileError("codegen: internal error: expression visited by statement visitor");
  }
  void LLVMGenerator::StatementVisitor::visit(const ast::FieldAccessExpression&) {
    throw CompileError("codegen: internal error: expression visited by statement visitor");
  }

  LLVMGenerator::ExpressionVisitor::ExpressionVisitor(const LLVMGenerator& generator,
                                                      IREmitter& emitter, CodegenContext& context,
                                                      const std::vector<Scope>& scopes)
      : generator_(generator), emitter_(emitter), context_(context), scopes_(scopes) {}

  void LLVMGenerator::ExpressionVisitor::visit(const ast::IntegerLiteral& integer) {
    result_ = Value{std::to_string(integer.value), Type::i32()};
  }

  void LLVMGenerator::ExpressionVisitor::visit(const ast::FloatLiteral& floating) {
    std::ostringstream literal;
    literal << floating.value;
    std::string text = literal.str();
    if (text.find('.') == std::string::npos && text.find('e') == std::string::npos &&
        text.find('E') == std::string::npos) {
      text += ".0";
    }
    result_ = Value{text, Type::f64()};
  }

  void LLVMGenerator::ExpressionVisitor::visit(const ast::StringLiteral& stringLiteral) {
    result_ = generator_.generateStringLiteral(stringLiteral, emitter_, context_);
  }

  void LLVMGenerator::ExpressionVisitor::visit(const ast::BoolLiteral& boolean) {
    result_ = Value{boolean.value ? "true" : "false", Type::boolean()};
  }

  void LLVMGenerator::ExpressionVisitor::visit(const ast::UnaryExpression& unary) {
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

  void LLVMGenerator::ExpressionVisitor::visit(const ast::CastExpression& castExpression) {
    result_ = generator_.generateCastExpression(castExpression, emitter_, context_, scopes_);
  }

  void LLVMGenerator::ExpressionVisitor::visit(const ast::BinaryExpression& binary) {
    result_ = generator_.generateBinaryExpression(binary, emitter_, context_, scopes_);
  }

  void LLVMGenerator::ExpressionVisitor::visit(const ast::IdentifierExpression& identifier) {
    const LocalBinding& local = generator_.lookupLocal(scopes_, identifier.name);

    const std::string result = emitter_.freshTemp();
    emitter_.emitLoad(local.type, local.slot, result);
    result_ = Value{result, local.type};
  }

  void LLVMGenerator::ExpressionVisitor::visit(const ast::CallExpression& call) {
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

  void LLVMGenerator::ExpressionVisitor::visit(const ast::ArrayLiteral& literal) {
    result_ = generator_.generateArrayLiteral(literal, emitter_, context_, scopes_);
  }

  void LLVMGenerator::ExpressionVisitor::visit(const ast::IndexExpression& index) {
    result_ = generator_.generateIndexExpression(index, emitter_, context_, scopes_);
  }

  void LLVMGenerator::ExpressionVisitor::visit(const ast::StructLiteral& literal) {
    result_ = generator_.generateStructLiteral(literal, emitter_, context_, scopes_);
  }

  void LLVMGenerator::ExpressionVisitor::visit(const ast::FieldAccessExpression& access) {
    result_ = generator_.generateFieldAccess(access, emitter_, context_, scopes_);
  }

  void LLVMGenerator::ExpressionVisitor::visit(const ast::ReturnStatement&) {
    throw CompileError("codegen: internal error: statement visited by expression visitor");
  }
  void LLVMGenerator::ExpressionVisitor::visit(const ast::LetStatement&) {
    throw CompileError("codegen: internal error: statement visited by expression visitor");
  }
  void LLVMGenerator::ExpressionVisitor::visit(const ast::IfStatement&) {
    throw CompileError("codegen: internal error: statement visited by expression visitor");
  }
  void LLVMGenerator::ExpressionVisitor::visit(const ast::WhileStatement&) {
    throw CompileError("codegen: internal error: statement visited by expression visitor");
  }
  void LLVMGenerator::ExpressionVisitor::visit(const ast::AssignmentStatement&) {
    throw CompileError("codegen: internal error: statement visited by expression visitor");
  }
  void LLVMGenerator::ExpressionVisitor::visit(const ast::ExpressionStatement&) {
    throw CompileError("codegen: internal error: statement visited by expression visitor");
  }

  LLVMGenerator::PlaceVisitor::PlaceVisitor(const LLVMGenerator& generator, IREmitter& emitter,
                                            CodegenContext& context,
                                            const std::vector<Scope>& scopes)
      : generator_(generator), emitter_(emitter), context_(context), scopes_(scopes) {}

  void LLVMGenerator::PlaceVisitor::visit(const ast::IdentifierExpression& identifier) {
    result_ = generator_.lookupLocal(scopes_, identifier.name);
  }

  void LLVMGenerator::PlaceVisitor::visit(const ast::IntegerLiteral&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlaceVisitor::visit(const ast::FloatLiteral&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlaceVisitor::visit(const ast::StringLiteral&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlaceVisitor::visit(const ast::BoolLiteral&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlaceVisitor::visit(const ast::UnaryExpression&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlaceVisitor::visit(const ast::CastExpression&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlaceVisitor::visit(const ast::BinaryExpression&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlaceVisitor::visit(const ast::CallExpression&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlaceVisitor::visit(const ast::ArrayLiteral&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlaceVisitor::visit(const ast::StructLiteral&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlaceVisitor::visit(const ast::IndexExpression& index) {
    const Value base = generator_.generateRvalue(*index.base, emitter_, context_, scopes_);
    const Value indexValue = generator_.generateRvalue(*index.index, emitter_, context_, scopes_);

    if (base.type.kind != TypeKind::Array) {
      throw CompileError("codegen: invalid assignment target");
    }
    if (!base.type.element) {
      throw CompileError("codegen: array type missing element type");
    }

    const Type elementType = *base.type.element;
    const std::string pointer =
        generator_.emitArrayElementPointer(base, indexValue, elementType, emitter_, context_);
    result_ = LocalBinding{pointer, elementType, true};
  }

  void LLVMGenerator::PlaceVisitor::visit(const ast::FieldAccessExpression& access) {
    const LocalBinding base = generator_.generatePlace(*access.base, emitter_, context_, scopes_);
    if (base.type.kind != TypeKind::Struct) {
      throw CompileError("codegen: field access requires struct base");
    }

    const StructLayout& layout = generator_.lookupStructLayout(context_, base.type);
    const auto field = layout.fieldIndex.find(access.fieldName);
    if (field == layout.fieldIndex.end()) {
      throw CompileError("codegen: struct '" + base.type.structName + "' has no field '" +
                         access.fieldName + "'");
    }

    result_ = LocalBinding{
        generator_.emitStructFieldPointer(base.type, base.slot, field->second, emitter_),
        layout.fieldTypes[field->second]};
  }

  void LLVMGenerator::PlaceVisitor::visit(const ast::ReturnStatement&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlaceVisitor::visit(const ast::LetStatement&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlaceVisitor::visit(const ast::IfStatement&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlaceVisitor::visit(const ast::WhileStatement&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlaceVisitor::visit(const ast::AssignmentStatement&) {
    throw CompileError("codegen: invalid assignment target");
  }
  void LLVMGenerator::PlaceVisitor::visit(const ast::ExpressionStatement&) {
    throw CompileError("codegen: invalid assignment target");
  }

  LLVMGenerator::LocalBinding LLVMGenerator::generatePlace(const ast::Expression& place,
                                                           IREmitter& emitter,
                                                           CodegenContext& context,
                                                           const std::vector<Scope>& scopes) const {
    PlaceVisitor visitor(*this, emitter, context, scopes);
    place.accept(visitor);
    return visitor.result();
  }

  std::string LLVMGenerator::emitArrayElementPointer(const Value& base, const Value& indexValue,
                                                     const Type& elementType, IREmitter& emitter,
                                                     CodegenContext& context) const {
    const std::string length = emitter.freshTemp();
    emitter.line(length + " = load i64, ptr " + base.text);
    emitBoundsCheck(length, indexValue, emitter, context, "array index out of bounds\n");

    const std::string elems = emitter.freshTemp();
    emitter.line(elems + " = getelementptr inbounds i8, ptr " + base.text + ", i64 8");
    return emitRawBufferElementPointer(Value{elems, Type::rawPtr()}, indexValue, elementType,
                                       emitter);
  }

  std::string LLVMGenerator::emitCStringPointer(std::string_view text, IREmitter& emitter,
                                                CodegenContext& context) const {
    const std::string globalName = "@.str." + std::to_string(context.nextStringGlobal++);
    const std::size_t length = text.size() + 1;
    context.globals << globalName << " = private unnamed_addr constant [" << length << " x i8] c\""
                    << escapeForLLVMString(text) << "\\00\"\n";

    const std::string result = emitter.freshTemp();
    emitter.line(result + " = getelementptr inbounds [" + std::to_string(length) + " x i8], ptr " +
                 globalName + ", i32 0, i32 0");
    return result;
  }

  void LLVMGenerator::emitRuntimeTrap(IREmitter& emitter, CodegenContext& context,
                                      std::string_view message) const {
    const std::string pointer = emitCStringPointer(message, emitter, context);
    emitter.line("call void @\"__noria.rt.trap\"(ptr " + pointer + ")");
    emitter.line("unreachable");
  }

  void LLVMGenerator::emitNullPointerCheck(const std::string& pointer, IREmitter& emitter,
                                           CodegenContext& context) const {
    const int labelId = emitter.freshLabelId();
    const std::string trapLabel = "alloc.fail" + std::to_string(labelId);
    const std::string contLabel = "alloc.ok" + std::to_string(labelId);
    const std::string isNull = emitter.freshTemp();
    emitter.line(isNull + " = icmp eq ptr " + pointer + ", null");
    emitter.emitCondBranch(isNull, trapLabel, contLabel);
    emitter.emitLabel(trapLabel);
    emitRuntimeTrap(emitter, context, "allocation failed\n");
    emitter.emitLabel(contLabel);
  }

  std::string LLVMGenerator::emitCheckedMalloc(const std::string& size64, IREmitter& emitter,
                                               CodegenContext& context) const {
    const std::string pointer = emitter.freshTemp();
    emitter.line(pointer + " = call ptr @malloc(i64 " + size64 + ")");
    emitNullPointerCheck(pointer, emitter, context);
    return pointer;
  }

  void LLVMGenerator::emitBoundsCheck(const std::string& length64, const Value& indexValue,
                                      IREmitter& emitter, CodegenContext& context,
                                      std::string_view message) const {
    const int labelId = emitter.freshLabelId();
    const std::string trapLabel = "bounds.fail" + std::to_string(labelId);
    const std::string contLabel = "bounds.ok" + std::to_string(labelId);
    const std::string index64 = emitter.freshTemp();
    emitter.line(index64 + " = zext i32 " + indexValue.text + " to i64");
    const std::string inBounds = emitter.freshTemp();
    emitter.line(inBounds + " = icmp ult i64 " + index64 + ", " + length64);
    emitter.emitCondBranch(inBounds, contLabel, trapLabel);
    emitter.emitLabel(trapLabel);
    emitRuntimeTrap(emitter, context, message);
    emitter.emitLabel(contLabel);
  }

  std::string LLVMGenerator::emitRawBufferElementPointer(const Value& base, const Value& indexValue,
                                                         const Type& elementType,
                                                         IREmitter& emitter) const {
    const std::size_t size = elementSizeInBytes(elementType);
    const std::string offset = emitter.freshTemp();
    emitter.line(offset + " = mul i32 " + indexValue.text + ", " + std::to_string(size));
    const std::string pointer = emitter.freshTemp();
    emitter.line(pointer + " = getelementptr i8, ptr " + base.text + ", i32 " + offset);
    return pointer;
  }

  std::string LLVMGenerator::emitBufferLoad(const Type& type, const std::string& pointer,
                                            IREmitter& emitter) const {
    if (type.kind == TypeKind::Bool) {
      const std::string packed = emitter.freshTemp();
      emitter.line(packed + " = load i8, ptr " + pointer);
      const std::string result = emitter.freshTemp();
      emitter.line(result + " = icmp ne i8 " + packed + ", 0");
      return result;
    }

    const std::string result = emitter.freshTemp();
    emitter.line(result + " = load " + LLVMType(type) + ", ptr " + pointer);
    return result;
  }

  void LLVMGenerator::emitBufferStore(const Type& type, const std::string& value,
                                      const std::string& pointer, IREmitter& emitter) const {
    if (type.kind == TypeKind::Bool) {
      const std::string packed = emitter.freshTemp();
      emitter.line(packed + " = zext i1 " + value + " to i8");
      emitter.line("store i8 " + packed + ", ptr " + pointer);
      return;
    }

    emitter.line("store " + LLVMType(type) + " " + value + ", ptr " + pointer);
  }

  bool LLVMGenerator::generateStatement(const ast::Statement& statement, IREmitter& emitter,
                                        CodegenContext& context, Type expectedReturnType,
                                        std::vector<Scope>& scopes) const {
    StatementVisitor visitor(*this, emitter, context, expectedReturnType, scopes);
    statement.accept(visitor);
    return visitor.returned();
  }

  LLVMGenerator::Value LLVMGenerator::generateStringLiteral(const ast::StringLiteral& literal,
                                                            IREmitter& emitter,
                                                            CodegenContext& context) const {
    return Value{emitCStringPointer(literal.value, emitter, context), Type::str()};
  }

  LLVMGenerator::Value LLVMGenerator::generateArrayLiteral(const ast::ArrayLiteral& literal,
                                                           IREmitter& emitter,
                                                           CodegenContext& context,
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

  LLVMGenerator::Value
  LLVMGenerator::generateIndexExpression(const ast::IndexExpression& index, IREmitter& emitter,
                                         CodegenContext& context,
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

  LLVMGenerator::Value
  LLVMGenerator::generateCastExpression(const ast::CastExpression& cast, IREmitter& emitter,
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

  std::optional<LLVMGenerator::Value>
  LLVMGenerator::tryGenerateBuiltinCall(const ast::CallExpression& call, IREmitter& emitter,
                                        CodegenContext& context,
                                        const std::vector<Scope>& scopes) const {

    const BuiltinSignature* descriptor = lookupBuiltin(call.callee);
    if (descriptor == nullptr)
      return std::nullopt;

    if (auto builtinEmitter = builtinEmitterFor(descriptor->id)) {
      return (this->*(*builtinEmitter))(call, emitter, context, scopes);
    }

    return std::nullopt;
  }

  std::optional<LLVMGenerator::BuiltinEmitter>
  LLVMGenerator::builtinEmitterFor(BuiltinId id) const {
    static constexpr std::array<std::pair<BuiltinId, BuiltinEmitter>, 23> emitters{{
        {BuiltinId::Println, &LLVMGenerator::emitPrintlnBuiltin},
        {BuiltinId::Print, &LLVMGenerator::emitPrintBuiltin},
        {BuiltinId::PrintInt, &LLVMGenerator::emitPrintIntBuiltin},
        {BuiltinId::PrintFloat, &LLVMGenerator::emitPrintFloatBuiltin},
        {BuiltinId::PrintChar, &LLVMGenerator::emitPrintCharBuiltin},
        {BuiltinId::Sqrt, &LLVMGenerator::emitSqrtBuiltin},
        {BuiltinId::Pow, &LLVMGenerator::emitPowBuiltin},
        {BuiltinId::Len, &LLVMGenerator::emitLenBuiltin},
        {BuiltinId::RtAlloc, &LLVMGenerator::emitRtAllocBuiltin},
        {BuiltinId::RtRealloc, &LLVMGenerator::emitRtReallocBuiltin},
        {BuiltinId::RtRelease, &LLVMGenerator::emitRtReleaseBuiltin},
        {BuiltinId::RtSizeof, &LLVMGenerator::emitRtSizeofBuiltin},
        {BuiltinId::RtLoad, &LLVMGenerator::emitRtLoadBuiltin},
        {BuiltinId::RtStore, &LLVMGenerator::emitRtStoreBuiltin},
        {BuiltinId::RtLoadPtr, &LLVMGenerator::emitRtLoadPtrBuiltin},
        {BuiltinId::RtStorePtr, &LLVMGenerator::emitRtStorePtrBuiltin},
        {BuiltinId::RtLoadI32, &LLVMGenerator::emitRtLoadI32Builtin},
        {BuiltinId::RtStoreI32, &LLVMGenerator::emitRtStoreI32Builtin},
        {BuiltinId::RtTrap, &LLVMGenerator::emitRtTrapBuiltin},
        {BuiltinId::RtNull, &LLVMGenerator::emitRtNullBuiltin},
        {BuiltinId::RtPtrEq, &LLVMGenerator::emitRtPtrEqBuiltin},
        {BuiltinId::RtHash, &LLVMGenerator::emitRtHashBuiltin},
        {BuiltinId::RtByteOffset, &LLVMGenerator::emitRtByteOffsetBuiltin},
    }};

    for (const auto& [candidate, emitter] : emitters) {
      if (candidate == id) {
        return emitter;
      }
    }
    return std::nullopt;
  }

  LLVMGenerator::Value LLVMGenerator::emitPrintlnBuiltin(
      const ast::CallExpression&, IREmitter& emitter, CodegenContext&,
      const std::vector<Scope>&) const {
    emitter.line("call i32 @putchar(i32 10)");
    return Value{"", Type::voidType()};
  }

  LLVMGenerator::Value LLVMGenerator::emitPrintBuiltin(
      const ast::CallExpression& call, IREmitter& emitter, CodegenContext& context,
      const std::vector<Scope>& scopes) const {
    const Value argument = generateRvalue(*call.arguments[0], emitter, context, scopes);
    emitter.line("call i32 @puts(ptr " + argument.text + ")");
    return Value{"", Type::voidType()};
  }

  LLVMGenerator::Value LLVMGenerator::emitPrintIntBuiltin(
      const ast::CallExpression& call, IREmitter& emitter, CodegenContext& context,
      const std::vector<Scope>& scopes) const {
    const Value argument = generateRvalue(*call.arguments[0], emitter, context, scopes);
    emitter.line("call void @noria_print_int(i32 " + argument.text + ")");
    return Value{"", Type::voidType()};
  }

  LLVMGenerator::Value LLVMGenerator::emitPrintFloatBuiltin(
      const ast::CallExpression& call, IREmitter& emitter, CodegenContext& context,
      const std::vector<Scope>& scopes) const {
    const Value argument = generateRvalue(*call.arguments[0], emitter, context, scopes);
    const std::string formatPointer = emitter.freshTemp();
    emitter.line(formatPointer +
                 " = getelementptr inbounds [4 x i8], ptr @.fmt.float, i32 0, i32 0");
    emitter.line("call i32 @printf(ptr " + formatPointer + ", double " + argument.text + ")");
    return Value{"", Type::voidType()};
  }

  LLVMGenerator::Value LLVMGenerator::emitPrintCharBuiltin(
      const ast::CallExpression& call, IREmitter& emitter, CodegenContext& context,
      const std::vector<Scope>& scopes) const {
    const Value argument = generateRvalue(*call.arguments[0], emitter, context, scopes);
    emitter.line("call i32 @putchar(i32 " + argument.text + ")");
    return Value{"", Type::voidType()};
  }

  LLVMGenerator::Value LLVMGenerator::emitSqrtBuiltin(
      const ast::CallExpression& call, IREmitter& emitter, CodegenContext& context,
      const std::vector<Scope>& scopes) const {
    const Value argument = generateRvalue(*call.arguments[0], emitter, context, scopes);
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = call double @llvm.sqrt.f64(double " + argument.text + ")");
    return Value{result, Type::f64()};
  }

  LLVMGenerator::Value LLVMGenerator::emitPowBuiltin(
      const ast::CallExpression& call, IREmitter& emitter, CodegenContext& context,
      const std::vector<Scope>& scopes) const {
    const Value base = generateRvalue(*call.arguments[0], emitter, context, scopes);
    const Value exponent = generateRvalue(*call.arguments[1], emitter, context, scopes);
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = call double @llvm.pow.f64(double " + base.text + ", double " +
                 exponent.text + ")");
    return Value{result, Type::f64()};
  }

  LLVMGenerator::Value LLVMGenerator::emitLenBuiltin(
      const ast::CallExpression& call, IREmitter& emitter, CodegenContext& context,
      const std::vector<Scope>& scopes) const {
    const Value argument = generateRvalue(*call.arguments[0], emitter, context, scopes);
    if (argument.type == Type::str()) {
      const std::string length = emitter.freshTemp();
      emitter.line(length + " = call i64 @strlen(ptr " + argument.text + ")");
      const std::string result = emitter.freshTemp();
      emitter.line(result + " = trunc i64 " + length + " to i32");
      return Value{result, Type::i32()};
    }

    const std::string length = emitter.freshTemp();
    emitter.line(length + " = load i64, ptr " + argument.text);
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = trunc i64 " + length + " to i32");
    return Value{result, Type::i32()};
  }

  LLVMGenerator::Value LLVMGenerator::emitRtAllocBuiltin(
      const ast::CallExpression& call, IREmitter& emitter, CodegenContext& context,
      const std::vector<Scope>& scopes) const {
    const Value size = generateRvalue(*call.arguments[0], emitter, context, scopes);
    const std::string size64 = emitter.freshTemp();
    emitter.line(size64 + " = sext i32 " + size.text + " to i64");
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = call ptr @malloc(i64 " + size64 + ")");
    emitNullPointerCheck(result, emitter, context);
    return Value{result, Type::rawPtr()};
  }

  LLVMGenerator::Value LLVMGenerator::emitRtReallocBuiltin(
      const ast::CallExpression& call, IREmitter& emitter, CodegenContext& context,
      const std::vector<Scope>& scopes) const {
    const Value pointer = generateRvalue(*call.arguments[0], emitter, context, scopes);
    const Value size = generateRvalue(*call.arguments[1], emitter, context, scopes);
    const std::string size64 = emitter.freshTemp();
    emitter.line(size64 + " = sext i32 " + size.text + " to i64");
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = call ptr @realloc(ptr " + pointer.text + ", i64 " + size64 + ")");
    emitNullPointerCheck(result, emitter, context);
    return Value{result, Type::rawPtr()};
  }

  LLVMGenerator::Value LLVMGenerator::emitRtReleaseBuiltin(
      const ast::CallExpression& call, IREmitter& emitter, CodegenContext& context,
      const std::vector<Scope>& scopes) const {
    const Value pointer = generateRvalue(*call.arguments[0], emitter, context, scopes);
    emitter.line("call void @free(ptr " + pointer.text + ")");
    return Value{"", Type::voidType()};
  }

  LLVMGenerator::Value LLVMGenerator::emitRtSizeofBuiltin(
      const ast::CallExpression&, IREmitter& emitter, CodegenContext& context,
      const std::vector<Scope>&) const {
    const Type witness =
        resolveWitnessType(functionSpecializationTypeArgs_, context.currentFunctionName);
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = add i32 0, " + std::to_string(elementSizeInBytes(witness)));
    return Value{result, Type::i32()};
  }

  LLVMGenerator::Value LLVMGenerator::emitRtLoadBuiltin(
      const ast::CallExpression& call, IREmitter& emitter, CodegenContext& context,
      const std::vector<Scope>& scopes) const {
    const Type witness =
        resolveWitnessType(functionSpecializationTypeArgs_, context.currentFunctionName);
    const Value pointer = generateRvalue(*call.arguments[0], emitter, context, scopes);
    const Value index = generateRvalue(*call.arguments[1], emitter, context, scopes);
    const std::string elementPointer = emitRawBufferElementPointer(pointer, index, witness, emitter);
    const std::string loaded = emitBufferLoad(witness, elementPointer, emitter);
    return Value{loaded, witness};
  }

  LLVMGenerator::Value LLVMGenerator::emitRtStoreBuiltin(
      const ast::CallExpression& call, IREmitter& emitter, CodegenContext& context,
      const std::vector<Scope>& scopes) const {
    const Type witness =
        resolveWitnessType(functionSpecializationTypeArgs_, context.currentFunctionName);
    const Value pointer = generateRvalue(*call.arguments[0], emitter, context, scopes);
    const Value index = generateRvalue(*call.arguments[1], emitter, context, scopes);
    const Value value = generateRvalue(*call.arguments[2], emitter, context, scopes);
    const std::string elementPointer = emitRawBufferElementPointer(pointer, index, witness, emitter);
    emitBufferStore(witness, value.text, elementPointer, emitter);
    return Value{"", Type::voidType()};
  }

  LLVMGenerator::Value LLVMGenerator::emitRtLoadPtrBuiltin(
      const ast::CallExpression& call, IREmitter& emitter, CodegenContext& context,
      const std::vector<Scope>& scopes) const {
    const Value pointer = generateRvalue(*call.arguments[0], emitter, context, scopes);
    const Value index = generateRvalue(*call.arguments[1], emitter, context, scopes);
    const std::string elementPointer =
        emitRawBufferElementPointer(pointer, index, Type::rawPtr(), emitter);
    const std::string loaded = emitter.freshTemp();
    emitter.line(loaded + " = load ptr, ptr " + elementPointer);
    return Value{loaded, Type::rawPtr()};
  }

  LLVMGenerator::Value LLVMGenerator::emitRtStorePtrBuiltin(
      const ast::CallExpression& call, IREmitter& emitter, CodegenContext& context,
      const std::vector<Scope>& scopes) const {
    const Value pointer = generateRvalue(*call.arguments[0], emitter, context, scopes);
    const Value index = generateRvalue(*call.arguments[1], emitter, context, scopes);
    const Value value = generateRvalue(*call.arguments[2], emitter, context, scopes);
    const std::string elementPointer =
        emitRawBufferElementPointer(pointer, index, Type::rawPtr(), emitter);
    emitter.line("store ptr " + value.text + ", ptr " + elementPointer);
    return Value{"", Type::voidType()};
  }

  LLVMGenerator::Value LLVMGenerator::emitRtLoadI32Builtin(
      const ast::CallExpression& call, IREmitter& emitter, CodegenContext& context,
      const std::vector<Scope>& scopes) const {
    const Value pointer = generateRvalue(*call.arguments[0], emitter, context, scopes);
    const Value index = generateRvalue(*call.arguments[1], emitter, context, scopes);
    const std::string elementPointer =
        emitRawBufferElementPointer(pointer, index, Type::i32(), emitter);
    const std::string loaded = emitter.freshTemp();
    emitter.line(loaded + " = load i32, ptr " + elementPointer);
    return Value{loaded, Type::i32()};
  }

  LLVMGenerator::Value LLVMGenerator::emitRtStoreI32Builtin(
      const ast::CallExpression& call, IREmitter& emitter, CodegenContext& context,
      const std::vector<Scope>& scopes) const {
    const Value pointer = generateRvalue(*call.arguments[0], emitter, context, scopes);
    const Value index = generateRvalue(*call.arguments[1], emitter, context, scopes);
    const Value value = generateRvalue(*call.arguments[2], emitter, context, scopes);
    const std::string elementPointer =
        emitRawBufferElementPointer(pointer, index, Type::i32(), emitter);
    emitter.line("store i32 " + value.text + ", ptr " + elementPointer);
    return Value{"", Type::voidType()};
  }

  LLVMGenerator::Value LLVMGenerator::emitRtTrapBuiltin(
      const ast::CallExpression& call, IREmitter& emitter, CodegenContext& context,
      const std::vector<Scope>& scopes) const {
    const Value message = generateRvalue(*call.arguments[0], emitter, context, scopes);
    emitter.line("call void @\"__noria.rt.trap\"(ptr " + message.text + ")");
    return Value{"", Type::voidType()};
  }

  LLVMGenerator::Value LLVMGenerator::emitRtNullBuiltin(
      const ast::CallExpression&, IREmitter& emitter, CodegenContext&,
      const std::vector<Scope>&) const {
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = inttoptr i64 0 to ptr");
    return Value{result, Type::rawPtr()};
  }

  LLVMGenerator::Value LLVMGenerator::emitRtPtrEqBuiltin(
      const ast::CallExpression& call, IREmitter& emitter, CodegenContext& context,
      const std::vector<Scope>& scopes) const {
    const Value left = generateRvalue(*call.arguments[0], emitter, context, scopes);
    const Value right = generateRvalue(*call.arguments[1], emitter, context, scopes);
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = icmp eq ptr " + left.text + ", " + right.text);
    return Value{result, Type::boolean()};
  }

  LLVMGenerator::Value LLVMGenerator::emitRtHashBuiltin(
      const ast::CallExpression& call, IREmitter& emitter, CodegenContext& context,
      const std::vector<Scope>& scopes) const {
    const Type witness =
        resolveWitnessType(functionSpecializationTypeArgs_, context.currentFunctionName);
    const Value key = generateRvalue(*call.arguments[0], emitter, context, scopes);
    const std::string result = emitter.freshTemp();
    if (witness == Type::i32()) {
      const std::string mixed = emitter.freshTemp();
      emitter.line(mixed + " = mul i32 " + key.text + ", 2654435761");
      const std::string masked = emitter.freshTemp();
      emitter.line(masked + " = and i32 " + mixed + ", 2147483647");
      return Value{masked, Type::i32()};
    }
    if (witness == Type::boolean()) {
      emitter.line(result + " = zext i1 " + key.text + " to i32");
      return Value{result, Type::i32()};
    }
    if (witness == Type::str()) {
      emitter.line(result + " = call i32 @noria_hash_str(ptr " + key.text + ")");
      return Value{result, Type::i32()};
    }
    throw CompileError("codegen: __rt_hash unsupported witness type " + witness.name());
  }

  LLVMGenerator::Value LLVMGenerator::emitRtByteOffsetBuiltin(
      const ast::CallExpression& call, IREmitter& emitter, CodegenContext& context,
      const std::vector<Scope>& scopes) const {
    const Value pointer = generateRvalue(*call.arguments[0], emitter, context, scopes);
    const Value bytes = generateRvalue(*call.arguments[1], emitter, context, scopes);
    const std::string result = emitter.freshTemp();
    emitter.line(result + " = getelementptr i8, ptr " + pointer.text + ", i32 " + bytes.text);
    return Value{result, Type::rawPtr()};
  }

  std::string LLVMGenerator::generateCondition(const ast::Expression& expression,
                                               IREmitter& emitter, CodegenContext& context,
                                               const std::vector<Scope>& scopes) const {
    const Value value = generateRvalue(expression, emitter, context, scopes);
    if (value.type != Type::boolean())
      throw CompileError("codegen: condition must be bool");
    return value.text;
  }

  LLVMGenerator::Value LLVMGenerator::generateRvalue(const ast::Expression& expression,
                                                     IREmitter& emitter, CodegenContext& context,
                                                     const std::vector<Scope>& scopes) const {
    ExpressionVisitor visitor(*this, emitter, context, scopes);
    expression.accept(visitor);
    return visitor.result();
  }

  LLVMGenerator::Value
  LLVMGenerator::generateBinaryExpression(const ast::BinaryExpression& binary, IREmitter& emitter,
                                          CodegenContext& context,
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

  LLVMGenerator::Value LLVMGenerator::generateShortCircuitBinaryExpression(
      const ast::BinaryExpression& binary, IREmitter& emitter, CodegenContext& context,
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

  LLVMGenerator::Value LLVMGenerator::generateStringConcatExpression(
      const Value& left, const Value& right, IREmitter& emitter, CodegenContext& context) const {
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

  LLVMGenerator::Value LLVMGenerator::generateComparisonExpression(
      const ast::BinaryExpression& binary, const Value& left, const Value& right,
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
      emitter.line(compared + " = call i32 @strcmp(ptr " + left.text + ", ptr " + right.text +
                   ")");
      emitter.line(result + " = icmp " + std::string(info->LLVMIntegerPredicate) + " i32 " +
                   compared + ", 0");
      return Value{result, Type::boolean()};
    }

    const std::string integerType = left.type == Type::boolean() ? "i1" : "i32";
    emitter.line(result + " = icmp " + std::string(info->LLVMIntegerPredicate) + " " +
                 integerType + " " + left.text + ", " + right.text);
    return Value{result, Type::boolean()};
  }

  LLVMGenerator::Value LLVMGenerator::generateNumericBinaryExpression(
      const ast::BinaryExpression& binary, const Value& left, const Value& right,
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

    emitter.line(result + " = " + std::string(info->LLVMIntegerInstruction) + " i32 " +
                 left.text + ", " + right.text);
    return Value{result, Type::i32()};
  }

  bool LLVMGenerator::declareLocal(std::vector<Scope>& scopes, const std::string& name,
                                   LocalBinding binding) const {
    if (scopes.empty())
      scopes.emplace_back();

    auto& scope = scopes.back();
    if (scope.contains(name))
      return false;

    scope.emplace(name, std::move(binding));
    return true;
  }

  const LLVMGenerator::LocalBinding& LLVMGenerator::lookupLocal(const std::vector<Scope>& scopes,
                                                                const std::string& name) const {

    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
      const auto local = scope->find(name);
      if (local != scope->end())
        return local->second;
    }

    throw CompileError("codegen: unknown local variable '" + name + "'");
  }

  std::unordered_map<std::string, LLVMGenerator::FunctionBinding>
  LLVMGenerator::collectFunctionBindings(const ast::Module& module) const {
    std::unordered_map<std::string, FunctionBinding> functions;

    for (const auto& function : module.functions) {
      if (!function.typeParams.empty()) {
        continue;
      }

      FunctionBinding binding;
      binding.returnType = function.returnType;
      for (const auto& parameter : function.parameters) {
        binding.parameterTypes.push_back(parameter.type);
      }
      functions.emplace(function.name, std::move(binding));
    }

    return functions;
  }

  std::unordered_map<std::string, LLVMGenerator::StructLayout>
  LLVMGenerator::collectStructLayouts(const ast::Module& module) const {
    std::unordered_map<std::string, StructLayout> layouts;

    for (const auto& decl : module.structs) {
      if (!decl.typeParams.empty()) {
        continue;
      }
      StructLayout layout;
      for (const auto& field : decl.fields) {
        const std::size_t index = layout.fieldTypes.size();
        layout.fieldNames.push_back(field.name);
        layout.fieldTypes.push_back(field.type);
        layout.fieldIndex.emplace(field.name, index);
      }
      layouts.emplace(decl.name, std::move(layout));
    }

    return layouts;
  }

  std::string LLVMGenerator::emitStructTypeDefinitions(const ast::Module& module) const {
    std::ostringstream out;

    for (const auto& decl : module.structs) {
      if (!decl.typeParams.empty()) {
        continue;
      }
      out << "%" << decl.name << " = type { ";
      for (std::size_t index{}; index < decl.fields.size(); ++index) {
        if (index != 0)
          out << ", ";
        out << LLVMType(decl.fields[index].type);
      }
      out << " }\n";
    }

    return out.str();
  }

  const LLVMGenerator::StructLayout&
  LLVMGenerator::lookupStructLayout(const CodegenContext& context, const Type& structType) const {
    const auto layout = context.structs.find(structType.structName);
    if (layout == context.structs.end()) {
      throw CompileError("codegen: unknown struct '" + structType.structName + "'");
    }

    return layout->second;
  }

  std::string LLVMGenerator::emitStructFieldPointer(const Type& structType, const std::string& slot,
                                                    std::size_t fieldIndex,
                                                    IREmitter& emitter) const {
    const std::string pointer = emitter.freshTemp();
    emitter.line(pointer + " = getelementptr inbounds " + LLVMType(structType) + ", ptr " + slot +
                 ", i32 0, i32 " + std::to_string(fieldIndex));
    return pointer;
  }

  LLVMGenerator::Value
  LLVMGenerator::generateStructLiteral(const ast::StructLiteral& literal, IREmitter& emitter,
                                       CodegenContext& context,
                                       const std::vector<Scope>& scopes) const {
    const Type structType = Type::structType(literal.structName);
    const StructLayout& layout = lookupStructLayout(context, structType);

    std::unordered_map<std::string, const ast::Expression*> fieldValues;
    for (const auto& field : literal.fields) {
      fieldValues.emplace(field.name, field.value.get());
    }

    const std::string slot = emitter.freshTemp();
    emitter.emitAlloca(structType, slot);

    for (std::size_t index{}; index < layout.fieldTypes.size(); ++index) {
      const std::string& fieldName = layout.fieldNames[index];
      const auto valueExpression = fieldValues.find(fieldName);
      if (valueExpression == fieldValues.end()) {
        throw CompileError("codegen: missing struct literal field '" + fieldName + "'");
      }

      const Value fieldValue = generateRvalue(*valueExpression->second, emitter, context, scopes);
      const std::string pointer = emitStructFieldPointer(structType, slot, index, emitter);
      emitter.emitStore(layout.fieldTypes[index], fieldValue.text, pointer);
    }

    const std::string result = emitter.freshTemp();
    emitter.emitLoad(structType, slot, result);
    return Value{result, structType};
  }

  LLVMGenerator::Value LLVMGenerator::generateFieldAccess(const ast::FieldAccessExpression& access,
                                                          IREmitter& emitter,
                                                          CodegenContext& context,
                                                          const std::vector<Scope>& scopes) const {
    std::string slot;
    Type structType;

    if (const auto* identifier =
            dynamic_cast<const ast::IdentifierExpression*>(access.base.get())) {
      const LocalBinding& local = lookupLocal(scopes, identifier->name);
      if (local.type.kind != TypeKind::Struct) {
        throw CompileError("codegen: field access requires struct base");
      }
      slot = local.slot;
      structType = local.type;
    } else {
      const Value baseValue = generateRvalue(*access.base, emitter, context, scopes);
      if (baseValue.type.kind != TypeKind::Struct) {
        throw CompileError("codegen: field access requires struct base");
      }
      structType = baseValue.type;
      slot = emitter.freshTemp();
      emitter.emitAlloca(structType, slot);
      emitter.emitStore(structType, baseValue.text, slot);
    }

    const StructLayout& layout = lookupStructLayout(context, structType);
    const auto fieldIndex = layout.fieldIndex.find(access.fieldName);
    if (fieldIndex == layout.fieldIndex.end()) {
      throw CompileError("codegen: struct '" + structType.structName + "' has no field '" +
                         access.fieldName + "'");
    }

    const Type fieldType = layout.fieldTypes[fieldIndex->second];
    const std::string pointer =
        emitStructFieldPointer(structType, slot, fieldIndex->second, emitter);
    const std::string result = emitter.freshTemp();
    emitter.emitLoad(fieldType, pointer, result);
    return Value{result, fieldType};
  }

} // namespace noria
