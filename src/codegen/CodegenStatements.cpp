#include "CodegenState.hpp"

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

  LLVMGenerator::StatementsState::StatementVisitor::StatementVisitor(
      const StatementsState& state, IREmitter& emitter, FunctionCodegenContext& context,
      Type expectedReturnType, std::vector<Scope>& scopes)
      : StatementOnlyVisitor("codegen"), state_(state), emitter_(emitter),
        context_(context), expectedReturnType_(expectedReturnType), scopes_(scopes) {}

  void LLVMGenerator::StatementsState::StatementVisitor::visit(
      const ast::LetStatement& letStatement) {
    std::optional<Value> initializer;
    if (letStatement.initializer) {
      initializer = state_.generateRvalue(*letStatement.initializer, emitter_, context_, scopes_,
                                          letStatement.declaredType);
    }

    const Type localType =
        letStatement.declaredType ? *letStatement.declaredType : initializer->type;
    const int localId = emitter_.freshTempCounter();
    const std::string slot =
        "%" + letStatement.name + ".slot" + std::to_string(localId);

    emitter_.emitAlloca(localType, slot);

    LocalBinding binding{slot, localType, false, {}};
    if (state_.generator().typeNeedsDrop(localType, context_)) {
      binding.ownedSlot = "%" + letStatement.name + ".owned" + std::to_string(localId);
      emitter_.emitAlloca(Type::boolean(), binding.ownedSlot);
    }

    if (!state_.declareLocal(scopes_, letStatement.name, std::move(binding), context_)) {
      throw CompileError("codegen: duplicate local variable '" + letStatement.name + "'");
    }

    const LocalBinding& local = state_.lookupLocal(scopes_, letStatement.name);
    if (initializer) {
      Value stored = *initializer;
      if (letStatement.initializer &&
          (dynamic_cast<const ast::IdentifierExpression*>(letStatement.initializer.get()) !=
               nullptr ||
           dynamic_cast<const ast::FieldAccessExpression*>(letStatement.initializer.get()) !=
               nullptr) &&
          state_.generator().typeNeedsDrop(localType, context_)) {
        // Own-mode rvalue already deep-clones managed identifiers/fields; only clone
        // when the loaded value is still borrowed.
        if (!stored.owned) {
          stored = state_.generator().emitCloneValue(stored, emitter_, context_);
        }
      } else if (state_.generator().typeNeedsDrop(localType, context_) && !stored.owned) {
        stored.owned = true;
      }
      state_.generator().emitStoreManagedLocal(local, stored, emitter_, context_);
    } else {
      const Value defaultValue = state_.emitDefaultValue(localType, emitter_, context_);
      state_.generator().emitStoreManagedLocal(local, defaultValue, emitter_, context_);
    }
    returned_ = false;
  }

  void LLVMGenerator::StatementsState::StatementVisitor::visit(
      const ast::ReturnStatement& returnStatement) {
    if (!returnStatement.expression) {
      if (expectedReturnType_ != Type::voidType()) {
        throw CompileError("codegen: non-void function returned without a value");
      }
      state_.emitDropScopes(scopes_, emitter_, context_);
      emitter_.line("ret void");
      returned_ = true;
      return;
    }

    if (expectedReturnType_ == Type::voidType()) {
      throw CompileError("codegen: void function returned a value");
    }

    Value returnValue;
    if (state_.generator().typeNeedsDrop(expectedReturnType_, context_) &&
        dynamic_cast<const ast::IdentifierExpression*>(returnStatement.expression.get()) !=
            nullptr) {
      returnValue = state_.generateRvalue(*returnStatement.expression, emitter_, context_,
                                          scopes_, expectedReturnType_,
                                          LLVMGenerator::OwnershipMode::Borrow);
    } else {
      returnValue = state_.generateRvalue(*returnStatement.expression, emitter_, context_,
                                          scopes_, expectedReturnType_);
    }

    if (state_.generator().typeNeedsDrop(expectedReturnType_, context_)) {
      if (const auto* identifier =
              dynamic_cast<const ast::IdentifierExpression*>(returnStatement.expression.get())) {
        const LocalBinding& local = state_.generator().lookupLocal(scopes_, identifier->name);
        if (!local.ownedSlot.empty()) {
          const std::string owned = emitter_.freshTemp();
          emitter_.line(owned + " = load i1, ptr " + local.ownedSlot);
          const int labelId = emitter_.freshLabelId();
          const std::string moveLabel = "return.move" + std::to_string(labelId);
          const std::string cloneLabel = "return.clone" + std::to_string(labelId);
          const std::string readyLabel = "return.ready" + std::to_string(labelId);
          const std::string resultSlot = "return.result" + std::to_string(labelId);
          emitter_.emitAlloca(expectedReturnType_, "%" + resultSlot);
          emitter_.emitCondBranch(owned, moveLabel, cloneLabel);
          emitter_.emitLabel(moveLabel);
          emitter_.line("store i1 false, ptr " + local.ownedSlot);
          emitter_.emitStore(expectedReturnType_, returnValue.text, "%" + resultSlot);
          emitter_.emitBranch(readyLabel);
          emitter_.emitLabel(cloneLabel);
          const Value cloned =
              state_.generator().emitCloneValue(returnValue, emitter_, context_);
          emitter_.emitStore(expectedReturnType_, cloned.text, "%" + resultSlot);
          emitter_.emitBranch(readyLabel);
          emitter_.emitLabel(readyLabel);
          const std::string result = emitter_.freshTemp();
          emitter_.emitLoad(expectedReturnType_, "%" + resultSlot, result);
          returnValue = Value{result, expectedReturnType_, true};
        }
      } else if (!returnValue.owned) {
        returnValue = state_.generator().emitCloneValue(returnValue, emitter_, context_);
      } else {
        returnValue.owned = true;
      }
    }

    state_.emitDropScopes(scopes_, emitter_, context_);
    emitter_.line("ret " + LLVMType(expectedReturnType_) + " " + returnValue.text);
    returned_ = true;
  }

  void LLVMGenerator::StatementsState::StatementVisitor::visit(
      const ast::AssignmentStatement& assignmentStatement) {
    if (const auto* index =
            dynamic_cast<const ast::IndexExpression*>(assignmentStatement.lhs.get());
        index != nullptr && index->standardContainer) {
      state_.assignContainerIndex(*index, *assignmentStatement.rhs, emitter_, context_, scopes_);
      returned_ = false;
      return;
    }

    const LocalBinding local =
        state_.generatePlace(*assignmentStatement.lhs, emitter_, context_, scopes_);

    Value rvalue = state_.generateRvalue(*assignmentStatement.rhs, emitter_, context_, scopes_,
                                         local.type);

    if (local.byteBuffer && state_.generator().typeContainsManaged(local.type, context_)) {
      const std::string oldValue = state_.emitBufferLoad(local.type, local.slot, emitter_);
      state_.generator().emitDropValue(Value{oldValue, local.type, true}, emitter_, context_);
      if (state_.generator().typeNeedsDrop(local.type, context_) && !rvalue.owned) {
        rvalue = state_.generator().emitCloneValue(rvalue, emitter_, context_);
      }
      state_.emitBufferStore(local.type, rvalue.text, local.slot, emitter_);
      returned_ = false;
      return;
    }

    if (!local.ownedSlot.empty()) {
      state_.generator().emitDropLocal(local, emitter_, context_);
      if (dynamic_cast<const ast::IdentifierExpression*>(assignmentStatement.rhs.get()) !=
              nullptr ||
          dynamic_cast<const ast::FieldAccessExpression*>(assignmentStatement.rhs.get()) !=
              nullptr) {
        // Own-mode rvalue already deep-clones managed identifiers/fields.
        if (!rvalue.owned) {
          rvalue = state_.generator().emitCloneValue(rvalue, emitter_, context_);
        }
      } else if (!rvalue.owned) {
        rvalue.owned = true;
      }
      state_.generator().emitStoreManagedLocal(local, rvalue, emitter_, context_);
    } else if (local.byteBuffer) {
      state_.emitBufferStore(local.type, rvalue.text, local.slot, emitter_);
    } else {
      emitter_.emitStore(local.type, rvalue.text, local.slot);
    }
    returned_ = false;
  }

  void LLVMGenerator::StatementsState::StatementVisitor::visit(
      const ast::IfStatement& ifStatement) {
    const int labelId = emitter_.freshLabelId();
    const std::string thenLabel = "if.then" + std::to_string(labelId);
    const std::string elseLabel = "if.else" + std::to_string(labelId);
    const std::string endLabel = "if.end" + std::to_string(labelId);

    const std::string condition =
        state_.generateCondition(*ifStatement.condition, emitter_, context_, scopes_);
    emitter_.emitCondBranch(condition, thenLabel, elseLabel);

    emitter_.emitLabel(thenLabel);
    scopes_.emplace_back();
    const bool thenReturns = state_.generateStatements(ifStatement.thenBranch, emitter_,
                                                           context_, expectedReturnType_, scopes_);
    if (!thenReturns) {
      state_.emitDropScope(scopes_.back(), emitter_, context_);
    }
    scopes_.pop_back();

    if (!thenReturns)
      emitter_.emitBranch(endLabel);

    emitter_.emitLabel(elseLabel);
    scopes_.emplace_back();
    const bool elseReturns = state_.generateStatements(ifStatement.elseBranch, emitter_,
                                                           context_, expectedReturnType_, scopes_);
    if (!elseReturns) {
      state_.emitDropScope(scopes_.back(), emitter_, context_);
    }
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

  void LLVMGenerator::StatementsState::StatementVisitor::visit(
      const ast::WhileStatement& whileStatement) {
    const int labelId = emitter_.freshLabelId();
    const std::string conditionLabel = "while.cond" + std::to_string(labelId);
    const std::string bodyLabel = "while.body" + std::to_string(labelId);
    const std::string endLabel = "while.end" + std::to_string(labelId);

    emitter_.emitBranch(conditionLabel);

    emitter_.emitLabel(conditionLabel);
    const std::string condition =
        state_.generateCondition(*whileStatement.condition, emitter_, context_, scopes_);
    emitter_.emitCondBranch(condition, bodyLabel, endLabel);

    emitter_.emitLabel(bodyLabel);
    scopes_.emplace_back();
    const bool bodyReturns = state_.generateStatements(whileStatement.body, emitter_, context_,
                                                           expectedReturnType_, scopes_);
    if (!bodyReturns) {
      state_.emitDropScope(scopes_.back(), emitter_, context_);
    }
    scopes_.pop_back();
    if (!bodyReturns)
      emitter_.emitBranch(conditionLabel);

    emitter_.emitLabel(endLabel);
    returned_ = false;
  }

  void LLVMGenerator::StatementsState::StatementVisitor::visit(
      const ast::ExpressionStatement& expressionStatement) {
    const Value value =
        state_.generateRvalue(*expressionStatement.expression, emitter_, context_, scopes_);
    state_.generator().emitReleaseIfOwned(value, emitter_, context_);
    returned_ = false;
  }

  bool LLVMGenerator::StatementsState::generateStatements(
      const std::vector<std::unique_ptr<ast::Statement>>& statements, IREmitter& emitter,
      FunctionCodegenContext& context, Type expectedReturnType, std::vector<Scope>& scopes) const {
    for (const auto& statement : statements) {
      if (generator().generateStatement(*statement, emitter, context, expectedReturnType, scopes))
        return true;
    }
    return false;
  }

  bool LLVMGenerator::StatementsState::generateStatement(
      const ast::Statement& statement, IREmitter& emitter, FunctionCodegenContext& context,
      Type expectedReturnType, std::vector<Scope>& scopes) const {
    StatementVisitor visitor(*this, emitter, context, expectedReturnType, scopes);
    statement.accept(visitor);
    return visitor.returned();
  }

  bool LLVMGenerator::StatementsState::declareLocal(std::vector<Scope>& scopes,
                                                     const std::string& name,
                                                     LocalBinding binding,
                                                     FunctionCodegenContext& context) const {
    if (scopes.empty())
      scopes.emplace_back();

    auto& scope = scopes.back();
    if (scope.bindings.contains(name))
      return false;

    if (generator().typeContainsManaged(binding.type, context)) {
      scope.containsPtr = true;
    }

    scope.bindings.emplace(name, std::move(binding));
    return true;
  }

  const LLVMGenerator::LocalBinding&
  LLVMGenerator::StatementsState::lookupLocal(const std::vector<Scope>& scopes,
                                               const std::string& name) const {

    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
      const auto local = scope->bindings.find(name);
      if (local != scope->bindings.end())
        return local->second;
    }

    throw CompileError("codegen: unknown local variable '" + name + "'");
  }

  void LLVMGenerator::StatementsState::emitDropScope(Scope& scope, IREmitter& emitter,
                                                      FunctionCodegenContext& context) const {
    generator().emitDropScope(scope, emitter, context);
  }

  void LLVMGenerator::StatementsState::emitDropScopes(std::vector<Scope>& scopes,
                                                       IREmitter& emitter,
                                                       FunctionCodegenContext& context) const {
    generator().emitDropScopes(scopes, emitter, context);
  }

  void LLVMGenerator::StatementsState::assignContainerIndex(
      const ast::IndexExpression& index, const ast::Expression& rhs, IREmitter& emitter,
      FunctionCodegenContext& context, std::vector<Scope>& scopes) const {
    if (!index.standardContainer) {
      throw CompileError("codegen: missing container index assignment");
    }

    const LocalBinding destination = generatePlace(*index.base, emitter, context, scopes);
    std::string baseText;
    if (destination.byteBuffer) {
      baseText = emitBufferLoad(destination.type, destination.slot, emitter);
    } else {
      baseText = emitter.freshTemp();
      emitter.emitLoad(destination.type, destination.slot, baseText);
    }
    const Value base{baseText, destination.type};
    const Value indexValue = generateRvalue(*index.index, emitter, context, scopes);

    const StandardContainer container = index.standardContainer->first;
    const std::vector<Type>& typeArgs = index.standardContainer->second.typeArguments();
    const Type expected = container == StandardContainer::Sequence ? typeArgs[0] : typeArgs[1];
    const Value assigned = generateRvalue(rhs, emitter, context, scopes, expected);
    const ContainerOperation operation = container == StandardContainer::Sequence
                                             ? ContainerOperation::Set
                                             : ContainerOperation::Insert;
    (void)emitStandardContainerCall(container, operation, typeArgs, {base, indexValue, assigned},
                                  emitter, context);
    generator().emitReleaseIfOwned(assigned, emitter, context);
  }

} // namespace noria
