#include "CodegenInternal.hpp"

#include "noria/Diagnostic.hpp"

#include <string>

namespace noria::codegen_detail {

  StatementEmitter::StatementVisitor::StatementVisitor(const StatementEmitter& state,
                                                       IREmitter& emitter,
                                                       FunctionCodegenContext& context,
                                                       Type expectedReturnType)
      : StatementOnlyVisitor("codegen"), state_(state), emitter_(emitter), context_(context),
        expectedReturnType_(expectedReturnType) {}

  void StatementEmitter::StatementVisitor::visit(const ast::LetStatement& letStatement) {
    std::optional<Value> initializer;
    if (letStatement.initializer) {
      initializer = state_.expressions_.generateRvalue(*letStatement.initializer, emitter_,
                                                       context_, letStatement.declaredType);
    }

    const Type localType =
        letStatement.declaredType ? *letStatement.declaredType : initializer->type;
    const int localId = emitter_.freshTempCounter();
    const std::string slot = "%" + letStatement.name + ".slot" + std::to_string(localId);

    emitter_.emitAlloca(localType, slot);

    LocalBinding binding{slot, localType, false, {}};
    if (state_.ownership_.typeNeedsDrop(localType, context_)) {
      binding.ownedSlot = "%" + letStatement.name + ".owned" + std::to_string(localId);
      emitter_.emitAlloca(Type::boolean(), binding.ownedSlot);
    }

    if (!context_.declareLocal(letStatement.name, std::move(binding),
                               state_.ownership_.typeContainsManaged(localType, context_))) {
      throw CompileError("codegen: duplicate local variable '" + letStatement.name + "'");
    }

    const LocalBinding& local = context_.lookupLocal(letStatement.name);
    if (initializer) {
      Value stored = *initializer;
      if (letStatement.initializer &&
          (dynamic_cast<const ast::IdentifierExpression*>(letStatement.initializer.get()) !=
               nullptr ||
           dynamic_cast<const ast::FieldAccessExpression*>(letStatement.initializer.get()) !=
               nullptr) &&
          state_.ownership_.typeNeedsDrop(localType, context_)) {
        if (!stored.owned) {
          stored = state_.ownership_.emitCloneValue(stored, emitter_, context_);
        }
      } else if (state_.ownership_.typeNeedsDrop(localType, context_) && !stored.owned) {
        stored.owned = true;
      }
      state_.ownership_.emitStoreManagedLocal(local, stored, emitter_, context_);
    } else {
      const Value defaultValue = state_.module().emitDefaultValue(localType, emitter_, context_);
      state_.ownership_.emitStoreManagedLocal(local, defaultValue, emitter_, context_);
    }
    returned_ = false;
  }

  void StatementEmitter::StatementVisitor::visit(const ast::ReturnStatement& returnStatement) {
    if (!returnStatement.expression) {
      if (expectedReturnType_ != Type::voidType()) {
        throw CompileError("codegen: non-void function returned without a value");
      }
      state_.ownership_.emitDropScopes(context_.scopes, emitter_, context_);
      emitter_.line("ret void");
      returned_ = true;
      return;
    }

    if (expectedReturnType_ == Type::voidType()) {
      throw CompileError("codegen: void function returned a value");
    }

    Value returnValue;
    if (state_.ownership_.typeNeedsDrop(expectedReturnType_, context_) &&
        dynamic_cast<const ast::IdentifierExpression*>(returnStatement.expression.get()) !=
            nullptr) {
      returnValue = state_.expressions_.generateRvalue(*returnStatement.expression, emitter_,
                                                       context_, expectedReturnType_,
                                                       LLVMGenerator::OwnershipMode::Borrow);
    } else {
      returnValue = state_.expressions_.generateRvalue(*returnStatement.expression, emitter_,
                                                       context_, expectedReturnType_);
    }

    if (state_.ownership_.typeNeedsDrop(expectedReturnType_, context_)) {
      if (const auto* identifier =
              dynamic_cast<const ast::IdentifierExpression*>(returnStatement.expression.get())) {
        const LocalBinding& local = context_.lookupLocal(identifier->name);
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
          const Value cloned = state_.ownership_.emitCloneValue(returnValue, emitter_, context_);
          emitter_.emitStore(expectedReturnType_, cloned.text, "%" + resultSlot);
          emitter_.emitBranch(readyLabel);
          emitter_.emitLabel(readyLabel);
          const std::string result = emitter_.freshTemp();
          emitter_.emitLoad(expectedReturnType_, "%" + resultSlot, result);
          returnValue = Value{result, expectedReturnType_, true};
        }
      } else if (!returnValue.owned) {
        returnValue = state_.ownership_.emitCloneValue(returnValue, emitter_, context_);
      } else {
        returnValue.owned = true;
      }
    }

    state_.ownership_.emitDropScopes(context_.scopes, emitter_, context_);
    emitter_.line("ret " + LLVMType(expectedReturnType_) + " " + returnValue.text);
    returned_ = true;
  }

  void
  StatementEmitter::StatementVisitor::visit(const ast::AssignmentStatement& assignmentStatement) {
    if (const auto* index =
            dynamic_cast<const ast::IndexExpression*>(assignmentStatement.lhs.get());
        index != nullptr && index->standardContainer) {
      state_.assignContainerIndex(*index, *assignmentStatement.rhs, emitter_, context_);
      returned_ = false;
      return;
    }

    const LocalBinding local =
        state_.places_.generatePlace(*assignmentStatement.lhs, emitter_, context_);

    Value rvalue = state_.expressions_.generateRvalue(*assignmentStatement.rhs, emitter_, context_,
                                                      local.type);

    if (local.byteBuffer && state_.ownership_.typeContainsManaged(local.type, context_)) {
      const std::string oldValue = state_.memory_.emitBufferLoad(local.type, local.slot, emitter_);
      state_.ownership_.emitDropValue(Value{oldValue, local.type, true}, emitter_, context_);
      if (state_.ownership_.typeNeedsDrop(local.type, context_) && !rvalue.owned) {
        rvalue = state_.ownership_.emitCloneValue(rvalue, emitter_, context_);
      }
      state_.memory_.emitBufferStore(local.type, rvalue.text, local.slot, emitter_);
      returned_ = false;
      return;
    }

    if (!local.ownedSlot.empty()) {
      state_.ownership_.emitDropLocal(local, emitter_, context_);
      if (dynamic_cast<const ast::IdentifierExpression*>(assignmentStatement.rhs.get()) !=
              nullptr ||
          dynamic_cast<const ast::FieldAccessExpression*>(assignmentStatement.rhs.get()) !=
              nullptr) {
        if (!rvalue.owned) {
          rvalue = state_.ownership_.emitCloneValue(rvalue, emitter_, context_);
        }
      } else if (!rvalue.owned) {
        rvalue.owned = true;
      }
      state_.ownership_.emitStoreManagedLocal(local, rvalue, emitter_, context_);
    } else if (local.byteBuffer) {
      state_.memory_.emitBufferStore(local.type, rvalue.text, local.slot, emitter_);
    } else {
      emitter_.emitStore(local.type, rvalue.text, local.slot);
    }
    returned_ = false;
  }

  void StatementEmitter::StatementVisitor::visit(const ast::IfStatement& ifStatement) {
    const int labelId = emitter_.freshLabelId();
    const std::string thenLabel = "if.then" + std::to_string(labelId);
    const std::string elseLabel = "if.else" + std::to_string(labelId);
    const std::string endLabel = "if.end" + std::to_string(labelId);

    const std::string condition =
        state_.expressions_.generateCondition(*ifStatement.condition, emitter_, context_);
    emitter_.emitCondBranch(condition, thenLabel, elseLabel);

    emitter_.emitLabel(thenLabel);
    context_.scopes.emplace_back();
    const bool thenReturns =
        state_.generateStatements(ifStatement.thenBranch, emitter_, context_, expectedReturnType_);
    if (!thenReturns) {
      state_.ownership_.emitDropScope(context_.scopes.back(), emitter_, context_);
    }
    context_.scopes.pop_back();

    if (!thenReturns) {
      emitter_.emitBranch(endLabel);
    }

    emitter_.emitLabel(elseLabel);
    context_.scopes.emplace_back();
    const bool elseReturns =
        state_.generateStatements(ifStatement.elseBranch, emitter_, context_, expectedReturnType_);
    if (!elseReturns) {
      state_.ownership_.emitDropScope(context_.scopes.back(), emitter_, context_);
    }
    context_.scopes.pop_back();
    if (!elseReturns) {
      emitter_.emitBranch(endLabel);
    }

    if (!thenReturns || !elseReturns) {
      emitter_.emitLabel(endLabel);
      returned_ = false;
      return;
    }

    returned_ = true;
  }

  void StatementEmitter::StatementVisitor::visit(const ast::WhileStatement& whileStatement) {
    const int labelId = emitter_.freshLabelId();
    const std::string conditionLabel = "while.cond" + std::to_string(labelId);
    const std::string bodyLabel = "while.body" + std::to_string(labelId);
    const std::string endLabel = "while.end" + std::to_string(labelId);

    emitter_.emitBranch(conditionLabel);

    emitter_.emitLabel(conditionLabel);
    const std::string condition =
        state_.expressions_.generateCondition(*whileStatement.condition, emitter_, context_);
    emitter_.emitCondBranch(condition, bodyLabel, endLabel);

    emitter_.emitLabel(bodyLabel);
    context_.scopes.emplace_back();
    const bool bodyReturns =
        state_.generateStatements(whileStatement.body, emitter_, context_, expectedReturnType_);
    if (!bodyReturns) {
      state_.ownership_.emitDropScope(context_.scopes.back(), emitter_, context_);
    }
    context_.scopes.pop_back();
    if (!bodyReturns) {
      emitter_.emitBranch(conditionLabel);
    }

    emitter_.emitLabel(endLabel);
    returned_ = false;
  }

  void
  StatementEmitter::StatementVisitor::visit(const ast::ExpressionStatement& expressionStatement) {
    const Value value =
        state_.expressions_.generateRvalue(*expressionStatement.expression, emitter_, context_);
    state_.ownership_.emitReleaseIfOwned(value, emitter_, context_);
    returned_ = false;
  }

  bool StatementEmitter::generateStatements(
      const std::vector<std::unique_ptr<ast::Statement>>& statements, IREmitter& emitter,
      FunctionCodegenContext& context, Type expectedReturnType) const {
    for (const auto& statement : statements) {
      if (generateStatement(*statement, emitter, context, expectedReturnType)) {
        return true;
      }
    }
    return false;
  }

  bool StatementEmitter::generateStatement(const ast::Statement& statement, IREmitter& emitter,
                                           FunctionCodegenContext& context,
                                           Type expectedReturnType) const {
    StatementVisitor visitor(*this, emitter, context, expectedReturnType);
    statement.accept(visitor);
    return visitor.returned();
  }

  void StatementEmitter::assignContainerIndex(const ast::IndexExpression& index,
                                              const ast::Expression& rhs, IREmitter& emitter,
                                              FunctionCodegenContext& context) const {
    if (!index.standardContainer) {
      throw CompileError("codegen: missing container index assignment");
    }

    const LocalBinding destination = places_.generatePlace(*index.base, emitter, context);
    std::string baseText;
    if (destination.byteBuffer) {
      baseText = memory_.emitBufferLoad(destination.type, destination.slot, emitter);
    } else {
      baseText = emitter.freshTemp();
      emitter.emitLoad(destination.type, destination.slot, baseText);
    }
    const Value base{baseText, destination.type};
    const Value indexValue = expressions_.generateRvalue(*index.index, emitter, context);

    const StandardContainer container = index.standardContainer->first;
    const std::vector<Type>& typeArgs = index.standardContainer->second.typeArguments();
    const Type expected = container == StandardContainer::Sequence ? typeArgs[0] : typeArgs[1];
    const Value assigned = expressions_.generateRvalue(rhs, emitter, context, expected);
    const ContainerOperation operation = container == StandardContainer::Sequence
                                             ? ContainerOperation::Set
                                             : ContainerOperation::Insert;
    (void)emitStandardContainerCall(container, operation, typeArgs, {base, indexValue, assigned},
                                    emitter, context);
    ownership_.emitReleaseIfOwned(assigned, emitter, context);
  }

} // namespace noria::codegen_detail
