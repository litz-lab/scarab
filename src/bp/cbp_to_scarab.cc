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
#include "mtage_unlimited.h"
/************************/

/******DO NOT MODIFY BELOW THIS POINT*****/

/**
 * @brief The template class below defines how all CBP predictors
 * interact with scarab.
 */

#include "bp/bp.param.h"
#include "cbp_to_scarab.h"

template <typename CBP_CLASS>
class CBP_To_Scarab_Intf {
  std::vector<CBP_CLASS> cbp_predictors;

 public:
  void init() {
    if (cbp_predictors.size() == 0) {
      cbp_predictors.reserve(NUM_CORES);
      for (uns i = 0; i < NUM_CORES; ++i) {
        cbp_predictors.emplace_back();
      }
    }
    ASSERTM(0, cbp_predictors.size() == NUM_CORES, "cbp_predictors not initialized correctly");
  }

  void timestamp(Op* op) {
    /* CBP Interface does not support speculative updates */
    op->recovery_info.branch_id = 0;
  }

  uns8 pred(Op* op) {
    uns proc_id = op->proc_id;
    if (op->off_path) return op->oracle_info.dir;
    return cbp_predictors.at(proc_id).GetPrediction(op->inst_info->addr, &op->bp_confidence);
  }

  void spec_update(Op* op) {
    /* CBP Interface does not support speculative updates */
    if (op->off_path) return;

    uns proc_id = op->proc_id;
    OpType optype = scarab_to_cbp_optype(op);

    if (is_conditional_branch(op)) {
      cbp_predictors.at(proc_id).UpdatePredictor(op->inst_info->addr, optype, op->oracle_info.dir, op->oracle_info.pred,
                                                 op->oracle_info.target);
    } else {
      cbp_predictors.at(proc_id).TrackOtherInst(op->inst_info->addr, optype, op->oracle_info.dir,
                                                op->oracle_info.target);
    }
  }

  void update(Op* op) { /* CBP Interface does not support update at exec */ }

  void retire(Op* op) {
    /* CBP Interface updates predictor at speculative update time */
  }

  void recover(Recovery_Info*) {
    /* CBP Interface does not support speculative updates */
  }

  Flag full(uns proc_id) {
    return cbp_predictors.at(proc_id).IsFull();
  }
};

// Specialization for TAGE64K
template <>
uns8 CBP_To_Scarab_Intf<TAGE64K>::pred(Op* op) {
  uns proc_id = op->proc_id;
  if (op->off_path)
    return op->oracle_info.dir;
  uns8 pred = cbp_predictors.at(proc_id).GetPrediction(op->inst_info->addr, &op->bp_confidence, op);

  return pred;
}

template <>
void CBP_To_Scarab_Intf<TAGE64K>::spec_update(Op* op) {
  uns proc_id = op->proc_id;
  OpType optype = scarab_to_cbp_optype(op);
  Flag is_conditional = is_conditional_branch(op);

  if (op->off_path) {
    return;
  }

  cbp_predictors.at(proc_id).SavePredictorStates();

  if (is_conditional) {
    cbp_predictors.at(proc_id).UpdatePredictor(op->inst_info->addr, optype, op->oracle_info.dir, op->oracle_info.pred,
                                               op->oracle_info.target);
  } else {
    cbp_predictors.at(proc_id).TrackOtherInst(op->inst_info->addr, optype, op->oracle_info.dir, op->oracle_info.target);
  }
}

template <>
void CBP_To_Scarab_Intf<TAGE64K>::timestamp(Op* op) {
  uns proc_id = op->proc_id;
  op->recovery_info.branch_id = cbp_predictors.at(proc_id).KeyGeneration(op->off_path);
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

#define DEF_CBP(CBP_NAME, CBP_CLASS)                                         \
  SCARAB_BP_INTF_FUNC_IMPL(CBP_CLASS, init, , void, , )                      \
  SCARAB_BP_INTF_FUNC_IMPL(CBP_CLASS, timestamp, , void, Op*, op)            \
  SCARAB_BP_INTF_FUNC_IMPL(CBP_CLASS, pred, return, uns8, Op*, op)           \
  SCARAB_BP_INTF_FUNC_IMPL(CBP_CLASS, spec_update, , void, Op*, op)          \
  SCARAB_BP_INTF_FUNC_IMPL(CBP_CLASS, update, , void, Op*, op)               \
  SCARAB_BP_INTF_FUNC_IMPL(CBP_CLASS, retire, , void, Op*, op)               \
  SCARAB_BP_INTF_FUNC_IMPL(CBP_CLASS, recover, , void, Recovery_Info*, info) \
  SCARAB_BP_INTF_FUNC_IMPL(CBP_CLASS, full, return, Flag, uns, proc_id)

#include "cbp_table.def"

#undef DEF_CBP
#undef SCARAB_BP_INF_FUNC_IMPL
#undef CBP_PREDICTOR
