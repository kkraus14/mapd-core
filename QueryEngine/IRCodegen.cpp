/*
 * Copyright 2017 MapD Technologies, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "Execute.h"
#include "MaxwellCodegenPatch.h"

// Driver methods for the IR generation.

std::vector<llvm::Value*> Executor::codegen(const Analyzer::Expr* expr,
                                            const bool fetch_columns,
                                            const CompilationOptions& co) {
  if (!expr) {
    return {posArg(expr)};
  }
  auto iter_expr = dynamic_cast<const Analyzer::IterExpr*>(expr);
  if (iter_expr) {
#ifdef ENABLE_MULTIFRAG_JOIN
    if (iter_expr->get_rte_idx() > 0) {
      const auto offset = cgen_state_->frag_offsets_[iter_expr->get_rte_idx()];
      if (offset) {
        return {cgen_state_->ir_builder_.CreateAdd(posArg(iter_expr), offset)};
      } else {
        return {posArg(iter_expr)};
      }
    }
#endif
    return {posArg(iter_expr)};
  }
  auto bin_oper = dynamic_cast<const Analyzer::BinOper*>(expr);
  if (bin_oper) {
    return {codegen(bin_oper, co)};
  }
  auto u_oper = dynamic_cast<const Analyzer::UOper*>(expr);
  if (u_oper) {
    return {codegen(u_oper, co)};
  }
  auto col_var = dynamic_cast<const Analyzer::ColumnVar*>(expr);
  if (col_var) {
    return codegen(col_var, fetch_columns, co);
  }
  auto constant = dynamic_cast<const Analyzer::Constant*>(expr);
  if (constant) {
    if (constant->get_is_null()) {
      const auto& ti = constant->get_type_info();
      return {ti.is_fp() ? static_cast<llvm::Value*>(inlineFpNull(ti)) : static_cast<llvm::Value*>(inlineIntNull(ti))};
    }
    // The dictionary encoding case should be handled by the parent expression
    // (cast, for now), here is too late to know the dictionary id
    CHECK_NE(kENCODING_DICT, constant->get_type_info().get_compression());
    return {codegen(constant, constant->get_type_info().get_compression(), 0, co)};
  }
  auto case_expr = dynamic_cast<const Analyzer::CaseExpr*>(expr);
  if (case_expr) {
    return {codegen(case_expr, co)};
  }
  auto extract_expr = dynamic_cast<const Analyzer::ExtractExpr*>(expr);
  if (extract_expr) {
    return {codegen(extract_expr, co)};
  }
  auto datediff_expr = dynamic_cast<const Analyzer::DatediffExpr*>(expr);
  if (datediff_expr) {
    return {codegen(datediff_expr, co)};
  }
  auto datetrunc_expr = dynamic_cast<const Analyzer::DatetruncExpr*>(expr);
  if (datetrunc_expr) {
    return {codegen(datetrunc_expr, co)};
  }
  auto charlength_expr = dynamic_cast<const Analyzer::CharLengthExpr*>(expr);
  if (charlength_expr) {
    return {codegen(charlength_expr, co)};
  }
  auto like_expr = dynamic_cast<const Analyzer::LikeExpr*>(expr);
  if (like_expr) {
    return {codegen(like_expr, co)};
  }
  auto regexp_expr = dynamic_cast<const Analyzer::RegexpExpr*>(expr);
  if (regexp_expr) {
    return {codegen(regexp_expr, co)};
  }
  auto likelihood_expr = dynamic_cast<const Analyzer::LikelihoodExpr*>(expr);
  if (likelihood_expr) {
    return {codegen(likelihood_expr->get_arg(), fetch_columns, co)};
  }
  auto in_expr = dynamic_cast<const Analyzer::InValues*>(expr);
  if (in_expr) {
    return {codegen(in_expr, co)};
  }
  auto in_integer_set_expr = dynamic_cast<const Analyzer::InIntegerSet*>(expr);
  if (in_integer_set_expr) {
    return {codegen(in_integer_set_expr, co)};
  }
  auto function_oper_with_custom_type_handling_expr =
      dynamic_cast<const Analyzer::FunctionOperWithCustomTypeHandling*>(expr);
  if (function_oper_with_custom_type_handling_expr) {
    return {codegenFunctionOperWithCustomTypeHandling(function_oper_with_custom_type_handling_expr, co)};
  }
  auto function_oper_expr = dynamic_cast<const Analyzer::FunctionOper*>(expr);
  if (function_oper_expr) {
    return {codegenFunctionOper(function_oper_expr, co)};
  }
  abort();
}

llvm::Value* Executor::codegen(const Analyzer::BinOper* bin_oper, const CompilationOptions& co) {
  const auto optype = bin_oper->get_optype();
  if (IS_ARITHMETIC(optype)) {
    return codegenArith(bin_oper, co);
  }
  if (IS_COMPARISON(optype)) {
    return codegenCmp(bin_oper, co);
  }
  if (IS_LOGIC(optype)) {
    return codegenLogical(bin_oper, co);
  }
  if (optype == kARRAY_AT) {
    return codegenArrayAt(bin_oper, co);
  }
  abort();
}

llvm::Value* Executor::codegen(const Analyzer::UOper* u_oper, const CompilationOptions& co) {
  const auto optype = u_oper->get_optype();
  switch (optype) {
    case kNOT:
      return codegenLogical(u_oper, co);
    case kCAST:
      return codegenCast(u_oper, co);
    case kUMINUS:
      return codegenUMinus(u_oper, co);
    case kISNULL:
      return codegenIsNull(u_oper, co);
    case kUNNEST:
      return codegenUnnest(u_oper, co);
    default:
      abort();
  }
}

llvm::Value* Executor::codegenRetOnHashFail(llvm::Value* hash_cond_lv, const Analyzer::Expr* qual) {
  std::unordered_map<const Analyzer::Expr*, size_t> equi_join_conds;
  for (size_t i = 0; i < plan_state_->join_info_.equi_join_tautologies_.size(); ++i) {
    auto cond = plan_state_->join_info_.equi_join_tautologies_[i];
    equi_join_conds.insert(std::make_pair(cond.get(), i));
  }
  auto bin_oper = dynamic_cast<const Analyzer::BinOper*>(qual);
  if (!bin_oper || !equi_join_conds.count(bin_oper)) {
    return hash_cond_lv;
  }

  auto bb_hash_pass = llvm::BasicBlock::Create(
      cgen_state_->context_, "hash_pass_" + std::to_string(equi_join_conds[bin_oper]), cgen_state_->row_func_);
  auto bb_hash_fail = llvm::BasicBlock::Create(
      cgen_state_->context_, "hash_fail_" + std::to_string(equi_join_conds[bin_oper]), cgen_state_->row_func_);
  cgen_state_->ir_builder_.CreateCondBr(hash_cond_lv, bb_hash_pass, bb_hash_fail);
  cgen_state_->ir_builder_.SetInsertPoint(bb_hash_fail);

  cgen_state_->ir_builder_.CreateRet(ll_int(int32_t(0)));
  cgen_state_->ir_builder_.SetInsertPoint(bb_hash_pass);
  return ll_bool(true);
}

const std::vector<Analyzer::Expr*> Executor::codegenHashJoinsBeforeLoopJoin(
    const std::vector<Analyzer::Expr*>& primary_quals,
    const RelAlgExecutionUnit& ra_exe_unit,
    const CompilationOptions& co) {
  std::unordered_set<const Analyzer::Expr*> hash_join_quals;
  if (plan_state_->join_info_.join_impl_type_ != JoinImplType::HashPlusLoop) {
    return primary_quals;
  }
  CHECK_GT(ra_exe_unit.input_descs.size(), size_t(2));
  const auto hash_join_count = ra_exe_unit.input_descs.size() - 2;

  llvm::Value* filter_lv = nullptr;
  for (auto expr : ra_exe_unit.inner_join_quals) {
    auto bin_oper = std::dynamic_pointer_cast<const Analyzer::BinOper>(expr);
    if (!bin_oper || bin_oper->get_optype() != kEQ) {
      continue;
    }
    std::set<int> rte_idx_set;
    bin_oper->collect_rte_idx(rte_idx_set);
    bool found_hash_join = true;
    for (auto rte : rte_idx_set) {
      if (rte >= static_cast<int>(hash_join_count)) {
        found_hash_join = false;
        break;
      }
    }
    if (!found_hash_join) {
      continue;
    }
    hash_join_quals.insert(expr.get());
    if (!filter_lv) {
      filter_lv = ll_bool(true);
    }
    CHECK(filter_lv);
    auto cond_lv = toBool(codegen(expr.get(), true, co).front());
    auto new_cond_lv = codegenRetOnHashFail(cond_lv, expr.get());
    filter_lv = new_cond_lv == cond_lv ? cgen_state_->ir_builder_.CreateAnd(filter_lv, cond_lv) : new_cond_lv;
    CHECK(filter_lv->getType()->isIntegerTy(1));
  }

  if (!filter_lv) {
    return primary_quals;
  }

  if (auto constant_true = dynamic_cast<llvm::ConstantInt*>(filter_lv)) {
    CHECK_NE(constant_true->getSExtValue(), int64_t(0));
  } else {
    auto cond_true = llvm::BasicBlock::Create(cgen_state_->context_, "match_true", cgen_state_->row_func_);
    auto cond_false = llvm::BasicBlock::Create(cgen_state_->context_, "match_false", cgen_state_->row_func_);

    cgen_state_->ir_builder_.CreateCondBr(filter_lv, cond_true, cond_false);
    cgen_state_->ir_builder_.SetInsertPoint(cond_false);
    cgen_state_->ir_builder_.CreateRet(ll_int(int32_t(0)));
    cgen_state_->ir_builder_.SetInsertPoint(cond_true);
  }

  std::vector<Analyzer::Expr*> remaining_quals;
  for (auto qual : primary_quals) {
    if (hash_join_quals.count(qual)) {
      continue;
    }
    remaining_quals.push_back(qual);
  }
  return remaining_quals;
}

void Executor::codegenInnerScanNextRow() {
  if (cgen_state_->inner_scan_labels_.empty()) {
    cgen_state_->ir_builder_.CreateRet(ll_int(int32_t(0)));
  } else {
    CHECK_EQ(size_t(1), cgen_state_->scan_to_iterator_.size());
    auto inner_it_val_and_ptr = cgen_state_->scan_to_iterator_.begin()->second;
    auto inner_it_inc = cgen_state_->ir_builder_.CreateAdd(inner_it_val_and_ptr.first, ll_int(int64_t(1)));
    cgen_state_->ir_builder_.CreateStore(inner_it_inc, inner_it_val_and_ptr.second);
    CHECK_EQ(size_t(1), cgen_state_->inner_scan_labels_.size());
    cgen_state_->ir_builder_.CreateBr(cgen_state_->inner_scan_labels_.front());
  }
}

Executor::GroupColLLVMValue Executor::groupByColumnCodegen(Analyzer::Expr* group_by_col,
                                                           const size_t col_width,
                                                           const CompilationOptions& co,
                                                           const bool translate_null_val,
                                                           const int64_t translated_null_val,
                                                           GroupByAndAggregate::DiamondCodegen& diamond_codegen,
                                                           std::stack<llvm::BasicBlock*>& array_loops,
                                                           const bool thread_mem_shared) {
#ifdef ENABLE_KEY_COMPACTION
  CHECK_GE(col_width, sizeof(int32_t));
#else
  CHECK_EQ(col_width, sizeof(int64_t));
#endif
  auto group_key = codegen(group_by_col, true, co).front();
  auto key_to_cache = group_key;
  if (dynamic_cast<Analyzer::UOper*>(group_by_col) &&
      static_cast<Analyzer::UOper*>(group_by_col)->get_optype() == kUNNEST) {
    auto preheader = cgen_state_->ir_builder_.GetInsertBlock();
    auto array_loop_head = llvm::BasicBlock::Create(
        cgen_state_->context_, "array_loop_head", cgen_state_->row_func_, preheader->getNextNode());
    diamond_codegen.setFalseTarget(array_loop_head);
    const auto ret_ty = get_int_type(32, cgen_state_->context_);
    auto array_idx_ptr = cgen_state_->ir_builder_.CreateAlloca(ret_ty);
    CHECK(array_idx_ptr);
    cgen_state_->ir_builder_.CreateStore(ll_int(int32_t(0)), array_idx_ptr);
    const auto arr_expr = static_cast<Analyzer::UOper*>(group_by_col)->get_operand();
    const auto& array_ti = arr_expr->get_type_info();
    CHECK(array_ti.is_array());
    const auto& elem_ti = array_ti.get_elem_type();
    auto array_len = cgen_state_->emitExternalCall(
        "array_size", ret_ty, {group_key, posArg(arr_expr), ll_int(log2_bytes(elem_ti.get_logical_size()))});
    cgen_state_->ir_builder_.CreateBr(array_loop_head);
    cgen_state_->ir_builder_.SetInsertPoint(array_loop_head);
    CHECK(array_len);
    auto array_idx = cgen_state_->ir_builder_.CreateLoad(array_idx_ptr);
    auto bound_check = cgen_state_->ir_builder_.CreateICmp(llvm::ICmpInst::ICMP_SLT, array_idx, array_len);
    auto array_loop_body = llvm::BasicBlock::Create(cgen_state_->context_, "array_loop_body", cgen_state_->row_func_);
    cgen_state_->ir_builder_.CreateCondBr(
        bound_check, array_loop_body, array_loops.empty() ? diamond_codegen.orig_cond_false_ : array_loops.top());
    cgen_state_->ir_builder_.SetInsertPoint(array_loop_body);
    cgen_state_->ir_builder_.CreateStore(cgen_state_->ir_builder_.CreateAdd(array_idx, ll_int(int32_t(1))),
                                         array_idx_ptr);
    const auto array_at_fname = "array_at_" + numeric_type_name(elem_ti);
    const auto ar_ret_ty = elem_ti.is_fp()
                               ? (elem_ti.get_type() == kDOUBLE ? llvm::Type::getDoubleTy(cgen_state_->context_)
                                                                : llvm::Type::getFloatTy(cgen_state_->context_))
                               : get_int_type(elem_ti.get_logical_size() * 8, cgen_state_->context_);
    group_key = cgen_state_->emitExternalCall(array_at_fname, ar_ret_ty, {group_key, posArg(arr_expr), array_idx});
    if (need_patch_unnest_double(elem_ti, isArchMaxwell(co.device_type_), thread_mem_shared)) {
      key_to_cache = spillDoubleElement(group_key, ar_ret_ty);
    } else {
      key_to_cache = group_key;
    }
    CHECK(array_loop_head);
    array_loops.push(array_loop_head);
  }
  cgen_state_->group_by_expr_cache_.push_back(key_to_cache);
  llvm::Value* orig_group_key{nullptr};
  if (translate_null_val) {
    const std::string translator_func_name(col_width == sizeof(int32_t) ? "translate_null_key_i32_"
                                                                        : "translate_null_key_");
    const auto& ti = group_by_col->get_type_info();
    const auto key_type = get_int_type(ti.get_logical_size() * 8, cgen_state_->context_);
    orig_group_key = group_key;
    group_key =
        cgen_state_->emitCall(translator_func_name + numeric_type_name(ti),
                              {group_key,
                               static_cast<llvm::Value*>(llvm::ConstantInt::get(key_type, inline_int_null_val(ti))),
                               static_cast<llvm::Value*>(llvm::ConstantInt::get(key_type, translated_null_val))});
  }
  group_key = cgen_state_->ir_builder_.CreateBitCast(castToTypeIn(group_key, col_width * 8),
                                                     get_int_type(col_width * 8, cgen_state_->context_));
  if (orig_group_key) {
    orig_group_key = cgen_state_->ir_builder_.CreateBitCast(castToTypeIn(orig_group_key, col_width * 8),
                                                            get_int_type(col_width * 8, cgen_state_->context_));
  }
  return {group_key, orig_group_key};
}
