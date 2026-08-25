#include "CodegenInternal.hpp"
#include "CodegenStrategy.hpp"

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

  LLVMGenerator::StatementVisitor::StatementVisitor(const LLVMGenerator& generator,
                                                    IREmitter& emitter,
                                                    FunctionCodegenContext& context,
                                                    Type expectedReturnType,
                                                    std::vector<Scope>& scopes)
      : StatementOnlyVisitor("codegen"), generator_(generator), emitter_(emitter),
        context_(context), expectedReturnType_(expectedReturnType), scopes_(scopes) {}

  void LLVMGenerator::StatementVisitor::visit(const ast::LetStatement& letStatement) {
    std::optional<Value> initializer;
    if (letStatement.initializer) {
      initializer = generator_.generateRvalue(*letStatement.initializer, emitter_, context_,
                                              scopes_, letStatement.declaredType);
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
      generator_.emitDefaultStore(localType, slot, emitter_, context_);
    }
    returned_ = false;
  }

  void LLVMGenerator::StatementVisitor::visit(const ast::ReturnStatement& returnStatement) {
    if (!returnStatement.expression) {
      if (expectedReturnType_ != Type::voidType()) {
        throw CompileError("codegen: non-void function returned without a value");
      }
      emitter_.line("ret void");
      returned_ = true;
      return;
    }

    if (expectedReturnType_ == Type::voidType()) {
      throw CompileError("codegen: void function returned a value");
    }

    Value returnValue = generator_.generateRvalue(*returnStatement.expression, emitter_, context_,
                                                  scopes_, expectedReturnType_);
    emitter_.line("ret " + LLVMType(expectedReturnType_) + " " + returnValue.text);
    returned_ = true;
  }

  void LLVMGenerator::StatementVisitor::visit(const ast::AssignmentStatement& assignmentStatement) {
    const LocalBinding local =
        generator_.generatePlace(*assignmentStatement.lhs, emitter_, context_, scopes_);

    Value rvalue = generator_.generateRvalue(*assignmentStatement.rhs, emitter_, context_, scopes_,
                                             local.type);
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

  bool LLVMGenerator::generateStatement(const ast::Statement& statement, IREmitter& emitter,
                                        FunctionCodegenContext& context, Type expectedReturnType,
                                        std::vector<Scope>& scopes) const {
    const auto strategy = activate(CodegenStrategyKind::Statements);
    StatementVisitor visitor(*this, emitter, context, expectedReturnType, scopes);
    statement.accept(visitor);
    return visitor.returned();
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

} // namespace noria
