/**
 * @file cbp_to_scarab.c
 * @author Stephen Pruett (stephen.pruett@utexas.edu)
 * @brief Implements the interface between CBP and Scarab
 * @version 0.1
 * @date 2021-01-12
 *
 * USAGE:
 *  Add your CBP header file to the list below.
 */

/**Add CBP Header Below**/
#include "cbp_tagescl_64k.h"
#include "decoupled_frontend.h"
#include "mtage_unlimited.h"
/************************/

/******DO NOT MODIFY BELOW THIS POINT*****/

/**
 * @brief The template class below defines how all CBP predictors
 * interact with scarab.
 */

#include "bp/bp.param.h"

#include "cbp_to_scarab.h"

static inline Bp_Pred_Info* cbp_get_bp_pred_info(Op* op, Bp_Pred_Level pred_level) {
  return (pred_level == BP_PRED_L0) ? &op->bp_pred_l0 : &op->bp_pred_main;
}

template <typename CBP_CLASS>
class CBP_To_Scarab_Intf {
  std::vector<std::vector<std::unique_ptr<CBP_CLASS>>> cbp_predictors_all_cores = {};

 public:
  CBP_CLASS* get_predictor(uns proc_id, uns bp_id) { return cbp_predictors_all_cores.at(proc_id).at(bp_id).get(); }

  void init() {
    if (NUM_BPS > 1)
      ASSERTM(0, SPEC_LEVEL == BP_PRED_ONOFF_SPEC_UPDATE_S_ONOFF_N_ON,
              "Multiple BPs are available for SPEC_LEVEL BP_PRED_ONOFF_SPEC_UPDATE_S_ONOFF_N_ON");

    if (cbp_predictors_all_cores.size() == 0) {
      cbp_predictors_all_cores.reserve(NUM_CORES);
      for (uns i = 0; i < NUM_CORES; ++i) {
        std::vector<std::unique_ptr<CBP_CLASS>> cbp_predictors;
        cbp_predictors.reserve(NUM_BPS);
        for (uns j = 0; j < NUM_BPS; ++j)
          cbp_predictors.emplace_back(std::make_unique<CBP_CLASS>());
        cbp_predictors_all_cores.emplace_back(std::move(cbp_predictors));
      }
    }
    for (uns i = 0; i < NUM_CORES; ++i) {
      ASSERTM(0, cbp_predictors_all_cores[i].size() == NUM_BPS, "cbp_predictors not initialized correctly");
    }
    ASSERTM(0, cbp_predictors_all_cores.size() == NUM_CORES, "cbp_predictors_all_cores not initialized correctly");
  }

  void timestamp(Op* op) {
    /* CBP Interface does not support speculative updates */
    op->recovery_info.branch_id = 0;
  }

  uns8 pred(Op* op, Bp_Pred_Level pred_level) {
    (void)pred_level;
    uns proc_id = op->proc_id;
    uns bp_id = op->parent_FT->get_bp_id();
    if (op->off_path)
      return op->oracle_info.dir;
    return cbp_predictors_all_cores.at(proc_id).at(bp_id)->GetPrediction(op->inst_info->addr, &op->bp_confidence);
  }

  void spec_update(Op* op, Bp_Pred_Level pred_level) {
    Bp_Pred_Info* bp_pred_info = cbp_get_bp_pred_info(op, pred_level);
    /* CBP Interface does not support speculative updates */
    if (op->off_path)
      return;

    uns proc_id = op->proc_id;
    uns bp_id = op->parent_FT->get_bp_id();
    OpType optype = scarab_to_cbp_optype(op->inst_info->table_info.cf_type);

    if (is_conditional_branch(op->inst_info->table_info.cf_type)) {
      cbp_predictors_all_cores.at(proc_id).at(bp_id)->UpdatePredictor(op->inst_info->addr, optype, op->oracle_info.dir,
                                                                      bp_pred_info->pred, op->oracle_info.target);
    } else {
      cbp_predictors_all_cores.at(proc_id).at(bp_id)->TrackOtherInst(op->inst_info->addr, optype, op->oracle_info.dir,
                                                                     op->oracle_info.target);
    }
  }

  void update(Op* op, Bp_Pred_Level pred_level) {
    (void)op;
    (void)pred_level;
    /* CBP Interface does not support update at exec */
  }

  void retire(Op* op) { /* CBP Interface updates predictor at speculative update time */ }

  void recover(Recovery_Info*) { /* CBP Interface does not support speculative updates */ }

  Flag full(Bp_Data* bp_data) { return cbp_predictors_all_cores.at(bp_data->proc_id).at(bp_data->bp_id)->IsFull(); }
};

// Specialization for TAGE64K
template <>
uns8 CBP_To_Scarab_Intf<TAGE64K>::pred(Op* op, Bp_Pred_Level pred_level) {
  (void)pred_level;
  uns proc_id = op->proc_id;
  uns bp_id = op->parent_FT->get_bp_id();
  if (op->off_path)
    if (SPEC_LEVEL < BP_PRED_ONOFF_SPEC_UPDATE_S_ONOFF_N_ON)
      return op->oracle_info.dir;
  uns8 pred =
      cbp_predictors_all_cores.at(proc_id).at(bp_id)->GetPrediction(op->inst_info->addr, &op->bp_confidence, op);

  return pred;
}

template <>
void CBP_To_Scarab_Intf<TAGE64K>::spec_update(Op* op, Bp_Pred_Level pred_level) {
  Bp_Pred_Info* bp_pred_info = cbp_get_bp_pred_info(op, pred_level);
  uns proc_id = op->proc_id;
  uns bp_id = op->parent_FT->get_bp_id();
  OpType optype = scarab_to_cbp_optype(op->inst_info->table_info.cf_type);
  Flag is_conditional = is_conditional_branch(op->inst_info->table_info.cf_type);
  Flag pred_dir =
      (SPEC_LEVEL < BP_PRED_ONOFF_SPEC_UPDATE_S_ONOFF_UPDATE_N_ON) ? op->oracle_info.dir : bp_pred_info->pred;
  const Flag l0_wrong = op->bp_pred_l0.recover_at_fe;
  const Flag main_wrong = op->bp_pred_main.recover_at_decode || op->bp_pred_main.recover_at_exec;
  // FE-only recovery (L0 wrong / main correct) still needs a main-BP
  // checkpoint because off-path speculative updates may already have been
  // applied before the late correction fires.
  const Flag fe_only_recovery = bp_l0_enabled() && l0_wrong && !main_wrong;
  const Flag checkpoint_needed =
      fe_only_recovery || op->bp_pred_main.recover_at_decode || op->bp_pred_main.recover_at_exec;

  if (op->off_path) {
    if (SPEC_LEVEL < BP_PRED_ON_SPEC_UPDATE_S_ONOFF_N_ON)
      return;
    if (is_conditional)
      cbp_predictors_all_cores.at(proc_id).at(bp_id)->SpecUpdateAtCond(op->inst_info->addr, pred_dir, true);
    cbp_predictors_all_cores.at(proc_id).at(bp_id)->SpecUpdate(op->inst_info->addr, optype, pred_dir,
                                                               op->oracle_info.target);
    return;
  }

  if (!bp_id) {
    cbp_predictors_all_cores.at(proc_id).at(bp_id)->SavePredictorStates(op->recovery_info.branch_id);
    if (!(SPEC_LEVEL < BP_PRED_ONOFF_SPEC_UPDATE_S_ONOFF_UPDATE_N_ON)) {
      if (checkpoint_needed) {
        ASSERT(op->proc_id, !op->off_path);
        cbp_predictors_all_cores.at(proc_id).at(bp_id)->TakeCheckpoint(op->recovery_info.branch_id);
      }
    }
  }

  // Real update start
  if (is_conditional) {
    cbp_predictors_all_cores.at(proc_id).at(bp_id)->SpecUpdateAtCond(op->inst_info->addr, pred_dir, false);
    cbp_predictors_all_cores.at(proc_id).at(bp_id)->SpecUpdate(op->inst_info->addr, optype, pred_dir,
                                                               op->oracle_info.target);
    if (SPEC_LEVEL < BP_PRED_ONOFF_SPEC_UPDATE_S_ONOFF_UPDATE_N_ON)
      cbp_predictors_all_cores.at(proc_id).at(bp_id)->NonSpecUpdateAtCond(
          op->inst_info->addr, optype, op->oracle_info.dir, bp_pred_info->pred, op->oracle_info.target,
          op->recovery_info.branch_id);
  } else {
    cbp_predictors_all_cores.at(proc_id).at(bp_id)->SpecUpdate(op->inst_info->addr, optype, pred_dir,
                                                               op->oracle_info.target);
    if (SPEC_LEVEL < BP_PRED_ONOFF_SPEC_UPDATE_S_ONOFF_UPDATE_N_ON)
      cbp_predictors_all_cores.at(proc_id).at(bp_id)->TrackOtherInst(op->inst_info->addr, optype, op->oracle_info.dir,
                                                                     op->oracle_info.target);
  }
  // Real update end

  if (bp_id)  // only take/verify checkpoints for the main BP
    return;

  if ((SPEC_LEVEL > BP_PRED_ON) && (SPEC_LEVEL < BP_PRED_ONOFF_SPEC_UPDATE_S_ONOFF_UPDATE_N_ON)) {
    if (checkpoint_needed) {
      ASSERT(op->proc_id, !op->off_path);
      cbp_predictors_all_cores.at(proc_id).at(bp_id)->TakeCheckpoint(op->recovery_info.branch_id);
      if (SPEC_LEVEL < BP_PRED_ON_SPEC_UPDATE_S_ONOFF_N_ON)
        cbp_predictors_all_cores.at(proc_id).at(bp_id)->VerifyPredictorStates(op->recovery_info.branch_id);
    }
  }
}

template <>
void CBP_To_Scarab_Intf<TAGE64K>::update(Op* op,
                                         Bp_Pred_Level pred_level) { /* CBP Interface does not support update at exec */
  Bp_Pred_Info* bp_pred_info = cbp_get_bp_pred_info(op, pred_level);
  if (op->off_path)
    return;
  if (SPEC_LEVEL < BP_PRED_ONOFF_SPEC_UPDATE_S_ONOFF_UPDATE_N_ON)
    return;

  uns proc_id = op->proc_id;
  uns bp_id = op->parent_FT->get_bp_id();
  OpType optype = scarab_to_cbp_optype(op->inst_info->table_info.cf_type);
  Flag is_conditional = is_conditional_branch(op->inst_info->table_info.cf_type);

  if (is_conditional)
    cbp_predictors_all_cores.at(proc_id).at(bp_id)->NonSpecUpdateAtCond(
        op->inst_info->addr, optype, op->oracle_info.dir, bp_pred_info->pred, op->oracle_info.target,
        op->recovery_info.branch_id);
  else
    cbp_predictors_all_cores.at(proc_id).at(bp_id)->TrackOtherInst(op->inst_info->addr, optype, op->oracle_info.dir,
                                                                   op->oracle_info.target);
}

template <>
void CBP_To_Scarab_Intf<TAGE64K>::retire(Op* op) {
  if (SPEC_LEVEL == BP_PRED_ON || op->parent_FT->get_bp_id())
    return;
  uns proc_id = op->proc_id;
  uns bp_id = op->parent_FT->get_bp_id();
  cbp_predictors_all_cores.at(proc_id).at(bp_id)->RetireCheckpoint(op->recovery_info.branch_id);
}

template <>
void CBP_To_Scarab_Intf<TAGE64K>::recover(Recovery_Info* recovery_info) {
  if (SPEC_LEVEL == BP_PRED_ON || recovery_info->bp_id)
    return;
  uns proc_id = recovery_info->proc_id;
  uns bp_id = recovery_info->bp_id;
  OpType optype = scarab_to_cbp_optype(recovery_info->cf_type);
  Flag is_conditional = is_conditional_branch(recovery_info->cf_type);
  cbp_predictors_all_cores.at(proc_id).at(bp_id)->RestoreStates(recovery_info->branch_id, recovery_info->PC, optype,
                                                                is_conditional, recovery_info->oracle_dir,
                                                                recovery_info->branchTarget);
}

template <>
void CBP_To_Scarab_Intf<TAGE64K>::timestamp(Op* op) {
  uns proc_id = op->proc_id;
  uns bp_id = op->parent_FT->get_bp_id();
  op->recovery_info.branch_id = cbp_predictors_all_cores.at(proc_id).at(bp_id)->KeyGeneration();
}

/******DO NOT MODIFY BELOW THIS POINT*****/

/**
 * @brief Macros below define c-style functions to interface with the
 * template class above. This way these functions can be called from C code.
 */

#define CBP_PREDICTOR(CBP_CLASS) cbp_predictor_##CBP_CLASS

#define DEF_CBP(CBP_NAME, CBP_CLASS) CBP_To_Scarab_Intf<CBP_CLASS> CBP_PREDICTOR(CBP_CLASS);
#include "cbp_table.def"
#undef DEF_CBP

#define SCARAB_BP_INTF_FUNC_IMPL(CBP_CLASS, FCN_NAME, Ret, RetType, Type, Arg) \
  RetType SCARAB_BP_INTF_FUNC(CBP_CLASS, FCN_NAME)(Type Arg) {                 \
    Ret CBP_PREDICTOR(CBP_CLASS).FCN_NAME(Arg);                                \
  }

#define SCARAB_BP_INTF_FUNC_IMPL2(CBP_CLASS, FCN_NAME, Ret, RetType, Type1, Arg1, Type2, Arg2) \
  RetType SCARAB_BP_INTF_FUNC(CBP_CLASS, FCN_NAME)(Type1 Arg1, Type2 Arg2) {                   \
    Ret CBP_PREDICTOR(CBP_CLASS).FCN_NAME(Arg1, Arg2);                                         \
  }

#define DEF_CBP(CBP_NAME, CBP_CLASS)                                                            \
  SCARAB_BP_INTF_FUNC_IMPL(CBP_CLASS, init, , void, , )                                         \
  SCARAB_BP_INTF_FUNC_IMPL(CBP_CLASS, timestamp, , void, Op*, op)                               \
  SCARAB_BP_INTF_FUNC_IMPL2(CBP_CLASS, pred, return, uns8, Op*, op, Bp_Pred_Level, pred_level)  \
  SCARAB_BP_INTF_FUNC_IMPL2(CBP_CLASS, spec_update, , void, Op*, op, Bp_Pred_Level, pred_level) \
  SCARAB_BP_INTF_FUNC_IMPL2(CBP_CLASS, update, , void, Op*, op, Bp_Pred_Level, pred_level)      \
  SCARAB_BP_INTF_FUNC_IMPL(CBP_CLASS, retire, , void, Op*, op)                                  \
  SCARAB_BP_INTF_FUNC_IMPL(CBP_CLASS, recover, , void, Recovery_Info*, info)                    \
  SCARAB_BP_INTF_FUNC_IMPL(CBP_CLASS, full, return, Flag, Bp_Data*, bp_data)

#include "cbp_table.def"

#undef DEF_CBP
#undef SCARAB_BP_INF_FUNC_IMPL
#undef SCARAB_BP_INTF_FUNC_IMPL2
#undef CBP_PREDICTOR

void bp_predictors_sync(Bp_Data* src, Bp_Data* dst) {
  TAGE64K* tage_src = cbp_predictor_TAGE64K.get_predictor(src->proc_id, src->bp_id);
  TAGE64K* tage_dst = cbp_predictor_TAGE64K.get_predictor(dst->proc_id, dst->bp_id);
  *tage_dst = *tage_src;
}

void bp_alt_spec_update_TAGE64K(uns proc_id, uns alt_bp_id, Op* trigger_op, Flag alt_dir) {
  ASSERT(0, alt_bp_id != 0);  // primary should not call this
  TAGE64K* alt_tage = cbp_predictor_TAGE64K.get_predictor(proc_id, alt_bp_id);
  OpType optype = scarab_to_cbp_optype(trigger_op->inst_info->table_info.cf_type);
  Flag is_conditional = is_conditional_branch(trigger_op->inst_info->table_info.cf_type);
  // Mirrors the spec/non-checkpoint half of CBP_To_Scarab_Intf<TAGE64K>::spec_update,
  // except: alt_dir overrides bp_pred_info->pred, and we skip SavePredictorStates
  // / TakeCheckpoint (those are gated on bp_id == 0 in the regular path; alt's
  // predictor doesn't track those structures).
  if (is_conditional)
    alt_tage->SpecUpdateAtCond(trigger_op->inst_info->addr, alt_dir, false);
  alt_tage->SpecUpdate(trigger_op->inst_info->addr, optype, alt_dir, trigger_op->oracle_info.target);
}
