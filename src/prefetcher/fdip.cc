#include "prefetcher/fdip.h"

#include <unordered_map>
#include <queue>
#include <algorithm>
#include <iostream>

extern "C" {
#include "debug/debug_macros.h"
#include "debug/debug_print.h"
#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "core.param.h"
#include "globals/assert.h"
#include "statistics.h"
#include "prefetcher/pref.param.h"
#include "memory/memory.param.h"
#include "memory/memory.h"

//#include "op.h"

//#include "table_info.h"
}

#define DUMMY_CF_NUM ~0

struct ftq_req {
    Addr target;
    Counter prefetch_cycle;
    Op op;
    bool prefetched; 
    bool taken;

    ftq_req(Addr _target, Counter _prefetch_cycle, Op _op, bool _prefetched, bool _taken)
	: target(_target),
	  prefetch_cycle(_prefetch_cycle), op(_op), prefetched(_prefetched), taken(_taken) {};
};

std::unordered_map<Addr, Op> pc_to_op;
std::queue<std::pair<Addr, ftq_req>> ftq;
Addr runahead_pc;
Icache_Stage *ic_stage;
Bp_Data *bp_data;
uns          cf_num    = 0;
bool runahead_disable = true;
Op *recovery_checkpoint;
bool recovery_checkpoint_valid = false;
bool on_wrong_path = false;

void fdip_init(Bp_Data* _bp_data,  Icache_Stage *_ic) {
    bp_data = _bp_data;
    ic_stage = _ic;
    ASSERT(0, ic_stage);
}

/* Called when a branch completes in the functional unit */
void fdip_resolve(Op *op) {
}

/* Called when a branch retires. If the branch retires that represents the
 * recovery checkpoint, the checkpoint is no longer valid. At this point we
 * do not have a valid recovery checkpoint. */
void fdip_retire(Op *op) {
    if (recovery_checkpoint_valid &&
	op->recovery_info.branch_id ==
	recovery_checkpoint->recovery_info.branch_id) {
	recovery_checkpoint_valid = false;
    }
}

/* When the branch is initially predicted by FDIP, a placeholder op struct is
 * used to store the predicted branch outcome, target, etc. Need to copy this
 * data into the current op
 */
void patch_oracle_info(Op *op, Op *req, Addr bp_pc) {
    //Copy placeholder recovery info into current op
    op->recovery_info = req->recovery_info;
    //Update dynamic fields of the current op that were unavailable for FDIP
    op->recovery_info.new_dir = op->oracle_info.dir;
    op->recovery_info.op_num = op->op_num;
    op->recovery_info.PC = op->inst_info->addr;
    ASSERT(0, op->recovery_info.cf_type == op->table_info->cf_type);
    op->recovery_info.oracle_dir = op->oracle_info.dir;
    op->recovery_info.branchTarget = op->oracle_info.target;

    Op_Info *op_info = &op->oracle_info;
    Op_Info *req_info = &req->oracle_info;

    op_info->pred_addr = bp_pc;
    op_info->btb_miss_resolved = req_info->btb_miss_resolved;

    op_info->pred_npc = req_info->pred_npc;   // predicted next pc field
    op_info->pred = req_info->pred;       // predicted direction of branch, set by the branch predictor

    op_info->btb_miss = req_info->btb_miss;           // true if the target is not known at prediction time
    op_info->no_target = req_info->no_target;  // true if there is no target for this branch at prediction

// FIXME late prefetcher
    //uns8 late_pred;  // predicted direction of branch, set by the multi-cycle
    // branch predictor
    //Addr late_pred_npc;  // predicted next pc field by the multi-cycle branch
    // predictor
    //Flag late_misfetch;  // true if target address is the ONLY thing that was
    // wrong after the multi-cycle branch prediction kicks in
    //Flag  late_mispred;  // true if the multi-cycle branch predictor mispredicted
    //Flag  recovery_sch;  // true if this op_info has scheduled a recovery
    op_info->pred_global_hist = req_info->pred_global_hist;  // global history used to predict the branch

// FIXME perceptron
    //uns64 pred_perceptron_global_hist;  // Only for perceptron, global history
    // used to predict the branch
    //uns64 pred_conf_perceptron_global_hist;  // Only for perceptron, global
    // history used to confidence predict
    // the branch
    //uns64 pred_conf_perceptron_global_misp_hist;  // Only for perceptron, global
    // history used to confidence
    // predict the branch uns32
    // pred_global_hist;      //
    // global history used to
    // predict the branch
    //uns8* pred_gpht_entry;  // entry used for interference free pred
    //uns8* pred_ppht_entry;  // entry used for interference free pred
    //uns8* pred_spht_entry;  // entry used for interference free pred
    op_info->pred_local_hist = req_info->pred_local_hist;  // local history used to predict the branch
    op_info->pred_targ_hist = req_info->pred_targ_hist;   // global history used to predict the indirect branch
    op_info->hybridgp_gpred = req_info->hybridgp_gpred;   // hybridgp's global prediction
    op_info->hybridgp_ppred = req_info->hybridgp_ppred;   // hybridgp's pred-address prediction
    op_info->pred_tc_selector_entry = req_info->pred_tc_selector_entry;  // which ibtb predicted this op_info?
    op_info->ibp_miss = req_info->ibp_miss;  // true if the target is not predicted by the indirect pred
}

/* Called when the frontend consumes the next prediction. There exist three
 * cases. a) FDIP has not predicted a branch yet after a recovery. 
 * b) The FDIP predicted branch matches the branch provided by the
 * frontend - consume the prediction. c) FDIP was on the wrong path and
 * predicted a branch that is never actually processed by the frontend
 */ 
Addr fdip_pred(Addr bp_pc, Op *op) {
    fdip_new_branch(op->fetch_addr, op);
    if (ftq.empty()) {
	/* If the FTQ is empty we have not been able to predict ahead. Should
	 * be a corner case e.g. directly after a recovery.
	 */
    	runahead_disable = false;
	Addr target = bp_predict_op(g_bp_data, op, cf_num++, bp_pc);
	bp_predict_op_evaluate(g_bp_data, op, target);
	recovery_checkpoint = op;
	recovery_checkpoint_valid = true;
	runahead_pc = target;
	on_wrong_path = false;
	STAT_EVENT(ic_stage->proc_id, FDIP_PRED_FTQ_EMPTY);
	return target;
    }
    else if (ftq.front().first == op->fetch_addr){
	/* FDIP predicted a branch on the right path. Consume the target and
	 * patch the current instruction.
	 */
	STAT_EVENT(ic_stage->proc_id, FDIP_PRED_CORRECT_PATH);
        auto req = &ftq.front().second;
	auto target = req->target;
	patch_oracle_info(op, &req->op, bp_pc);
	//Re-evaluate FDIP direction prediction based on the current oracle info
	auto target2 = bp_predict_op_evaluate(g_bp_data, op, req->target);
	ASSERT(0, req->target==target2);
	op->cf_within_fetch = cf_num++;
	if (!on_wrong_path) {
	    //We may have mispredicted once but the branch PCs seen by the
	    //frontend and FDIP still match
	    recovery_checkpoint = op;
	    recovery_checkpoint_valid = true;
	}
	/* A branch processed by FDIP has been mispredicted. We do not recover
	 * at this point as we want FDIP to continue prefetching on the wrong
	 * path. Recover only if this branch is resolved by the backend.
	 */
	ASSERT(0, req->taken == req->op.oracle_info.pred);
	if (op->oracle_info.mispred || op->oracle_info.misfetch) {
	    on_wrong_path = true;
	    ASSERT(0, op->oracle_info.mispred || op->oracle_info.misfetch);
	}
	else {
	    ASSERT(0, !op->oracle_info.mispred && !op->oracle_info.misfetch);
	}
	ASSERT(0,req->target);
	if (req->prefetched) {
	    STAT_EVENT(ic_stage->proc_id, FDIP_PREF_CORRECT_PATH);
	    INC_STAT_EVENT(ic_stage->proc_id, FDIP_SAVED_PREF_CYC,
			   cycle_count - req->prefetch_cycle);
	}
	ftq.pop();
	return target;
    }
    else {
	/* If the executed branch does not match the oldest entry in the FTQ,
	 * we are on the wrong path. Recover, clear FTQ and retry branch pred.
	 * At this point we may not have realized that we are on the wrong path
	 * as the branch has not been resolved yet. Because we are executing
	 * traces the frontend does not actually see branches on the wrong path.
	 */
	INC_STAT_EVENT(ic_stage->proc_id, FDIP_PRED_WRONG_PATH, ftq.size());
	if (recovery_checkpoint_valid) {
	    /* The frontend has fetched a branch and that branch is still
	     * executing. Hence we have a valid recovery point to which we will
	     * recover (squash all later branches predicted by FDIP).
	     */
	    bp_recover_op(g_bp_data, recovery_checkpoint->table_info->cf_type,
			  &recovery_checkpoint->recovery_info);
	} else {
	    /* MAYBE A HACK: The backend currently is not executing a branch. As
	     * a result we have no valid recovery point. FDIP predicted a branch
	     * that is never actually executed.
	     * First recover it so that all later branches are squashed then
	     * retire it like a regular instruction to keep the BP state in sync
	     * The BP history now contains an executed, mispredicted, retired
	     * branch that has never actually gone through the backend, this
	     * impact TAGE's accuracy (hopefully it doesn't happen often).
	     */
	    //Copy req onto the stack, as bp_recover_op will clear the ftq 
	    ftq_req req = ftq.front().second;
	    ASSERT(0, !ftq.empty());
	    bp_recover_op(g_bp_data, req.op.table_info->cf_type, &req.op.recovery_info);
	    bp_resolve_op(g_bp_data, &req.op);
	    bp_retire_op(g_bp_data, &req.op);
	    STAT_EVENT(ic_stage->proc_id, FDIP_SQUASH_FAKE_BRANCH);
	}
	fdip_clear_ftq(bp_pc);
	auto target =  bp_predict_op(g_bp_data, op, cf_num++, bp_pc);
	target = bp_predict_op_evaluate(g_bp_data, op, target);
	recovery_checkpoint = op;
	recovery_checkpoint_valid = true;
	runahead_disable = false;
	runahead_pc = target;
	on_wrong_path = false;

	return target;
    }
}

/* To enable runahead prefetching, maintain a data structure of all branches
 * that have been seen so far. This allows us to predict branches without 
 * receiving them from the simulator frontend.
 */
void fdip_new_branch(Addr bp_pc, Op *op) {
    if(pc_to_op.find(bp_pc) == pc_to_op.end()) {
	pc_to_op.insert(std::pair<Addr, Op>(bp_pc, *op));
    }
}

/* Clear the FTQ, branch predictor state needs to be recovered elsewhere
 */
void fdip_clear_ftq(Addr recover_pc) {
    runahead_pc = recover_pc;
    while (!ftq.empty()) {
	if (ftq.front().second.prefetched) {
	    STAT_EVENT(ic_stage->proc_id, FDIP_PREF_WRONG_PATH);
	}
	ftq.pop();
    }
}

/* When a mispredicted branch is resolved, the frontend recovers the branch
 * predictor state (including FDIP predictions), hence here we only have to 
 * clear the FTQ.
 */
void fdip_recover(Recovery_Info *info) {
    recovery_checkpoint = NULL;
    recovery_checkpoint_valid = false;
    fdip_clear_ftq(info->npc);
}

// Returns true if prefetch was emitted
bool fdip_prefetch(Addr target, Op *op) {
    Addr line_addr;

    auto line = (Inst_Info**)cache_access(&ic_stage->icache, target,
					  &line_addr, TRUE);
    if (!line) {
	if(new_mem_req(MRT_IFETCH, ic_stage->proc_id, line_addr,
		       ICACHE_LINE_SIZE, 0, NULL, icache_fill_line,
		       unique_count,
		       0)) {
	    STAT_EVENT(ic_stage->proc_id, FDIP_PREFETCHES);
	    return true;
	}
    }
    else {
	STAT_EVENT(ic_stage->proc_id, FDIP_PREF_ICACHE_HIT);
    }
    return false;
}

// Called each cycle to trigger runahead prefetches
void fdip_update() {
    Addr fdip_break_addr_top = ~0UL;
    Addr fdip_break_addr_bottom = 0UL;
    Addr CLMASK = 0x3F;
    uint32_t predicts = 0;
    uint32_t prefetches = 0;

    if (ftq.size() > FDIP_MAX_RUNAHEAD || runahead_disable) {
	return;
    }

    // Predict branches across cache lines. In many implementations, the BTB
    // is looked up once per cycle, returning all branches in the cache line
    if (FDIP_BREAK_ICACHE) {
	fdip_break_addr_top = runahead_pc | CLMASK;
	fdip_break_addr_bottom = runahead_pc & ~CLMASK;
    }

    // Find the next branch after the runahead PC. As the BTB/BP is addressed
    // with a PC and not cache line address we need to byte-wise increment the
    // runahead PC
    while (predicts < FDIP_BP_PER_CYC && prefetches < FDIP_PREF_PER_CYC &&
	   runahead_pc <= fdip_break_addr_top &&
	   runahead_pc >= fdip_break_addr_bottom) {
	auto op_iter = pc_to_op.find(runahead_pc);
	if (op_iter != pc_to_op.end()) {
	    Op *op = &op_iter->second;
	    ASSERT(ic_stage->proc_id, op->fetch_addr == runahead_pc);
	    auto target = bp_predict_op(g_bp_data, op, DUMMY_CF_NUM, runahead_pc);
	    ftq.push(std::pair<Addr, ftq_req>(runahead_pc, ftq_req(
						  target,
	    					  cycle_count, *op,
						  0, //prefetched
						  (op->oracle_info.pred == TAKEN))));
	    predicts++;
	    if (op->oracle_info.btb_miss || target == 0) {
		//On a BTB/RAS miss we cannot continue to FDIP
		runahead_disable = true;
		STAT_EVENT(ic_stage->proc_id, FDIP_BTB_RAS_MISS);
		return;
	    }
	    if (op->oracle_info.pred == TAKEN) {
		bool success = fdip_prefetch(target, op);
		prefetches += success;
		ftq.back().second.prefetched = success;
		runahead_pc = target;
		if (FDIP_BREAK_ICACHE) {
		    fdip_break_addr_top = runahead_pc | CLMASK;
		    fdip_break_addr_bottom = runahead_pc & ~CLMASK;
		}
	    }
	}
	runahead_pc++;
    }
}
