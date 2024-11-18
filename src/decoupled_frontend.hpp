#ifndef __DECOUPLED_FE_HPP_H__
#define __DECOUPLED_FE_HPP_H__

#include "decoupled_frontend.h"
#include "mp.hpp"

#include <vector>
#include <deque>
#include <memory>

class FT {
public:
  FT();
  FT(uns _proc_id);
  ~FT() {}
  void set_ft_started_by(FT_Started_By ft_started_by);
  void add_op(Op *op, FT_Ended_By ft_ended_by);
  void free_ops_and_clear();
  bool can_fetch_op();
  Op* fetch_op();
  void set_per_op_ft_info();

private:
  uns proc_id;
  // indicate the next op index to read by the consumer (icache or uop)
  uint64_t op_pos;
  FT_Info ft_info;
  std::vector<Op*> ops;

  friend class Decoupled_FE;
};

class Decoupled_FE {
public:
  Decoupled_FE() :
    off_path(0),
    sched_off_path(0),
    dfe_op_count(1),
    recovery_addr(0),
    redirect_cycle(0),
    stalled(false),
    ftq_ft_num(FE_FTQ_BLOCK_NUM),
    trace_mode(false),
    mp(nullptr) {}
  ~Decoupled_FE() {}
  void init(uns _proc_id, uns _bp_id, Bp_Data* bp_data, uns dfe_recovery_policy);
  int is_off_path() { return off_path; }
  void set_off_path() { off_path = true; }
  void recover(Cf_Type cf_type, Recovery_Info* info);
  void update();
  bool current_ft_can_fetch_op() { return current_ft_in_use.can_fetch_op(); }
  bool fill_icache_stage_data(int requested, Stage_Data *sd);
  bool can_fetch_ft() { return ftq.size() > 0; }
  FT_Info fetch_ft();
  FT_Info peek_ft();
  uns new_ftq_iter();
  Op* ftq_iter_get(uns iter_idx, bool* end_of_ft);
  Op* ftq_iter_get_next(uns iter_idx, bool* end_of_ft);
  uint64_t ftq_iter_offset(uns iter_idx);
  uint64_t ftq_iter_ft_offset(uns iter_idx);
  uint64_t ftq_num_ops();
  uint64_t ftq_num_fts() { return ftq.size(); }
  void stall(Op* op);
  void retire(Op* op, int op_proc_id, uns64 inst_uid);
  void set_ftq_num(uint64_t set_ftq_ft_num) { ftq_ft_num = set_ftq_ft_num; }
  uint64_t get_ftq_num() { return ftq_ft_num; }
  uns get_proc_id() { return proc_id; }
  uns get_bp_id() { return bp_id; }
  Bp_Data* get_bp_data() { return bp_data; }
  FT* get_last_fetch_target();
  uns get_dfe_recovery_policy() { return dfe_recovery_policy; }
  void insert_mp_candidate(FT_Info* ft_info, uns64 ghist) { mp->insert_mp_candidate(ft_info, ghist); }
  void search_mp_candidate(Addr line_addr) { mp->search_mp_candidate(line_addr); }
  MP* get_mp() { return mp; }
  Flag determine_to_prefetch_by_mp(Addr fetch_addr, uint64_t ghist);
  void set_last_cl_unuseful(Addr line_addr) { mp->set_last_cl_unuseful(line_addr); }
  Addr get_last_cl_unuseful() { return mp->get_last_cl_unuseful(); }

protected:
  void dfe_recover_op();
  Flag determine_to_run_alt_by_mp(Addr fetch_addr);

  uns proc_id;
  uns bp_id;
  Bp_Data* bp_data;
  uns dfe_recovery_policy;

  // Per core fetch target queue:
  // Each core has a queue of FTs,
  // where each FT contains a queue of micro instructions.
  std::deque<FT> ftq;
  // keep track of the current FT to be pushed next
  FT current_ft_to_push;
  // keep track of the current FT being used by the icache / uop cache
  FT current_ft_in_use;

  int off_path;
  int sched_off_path;
  uint64_t dfe_op_count;
  uint64_t recovery_addr;
  uint64_t redirect_cycle;
  bool stalled;
  uint64_t ftq_ft_num;
  bool trace_mode;
  std::vector<std::unique_ptr<decoupled_fe_iter>> ftq_iterators;
  MP* mp;
};

Flag decoupled_fe_determine_to_prefetch_by_mp(Addr fetch_addr, uint64_t ghist);
void decoupled_fe_set_last_cl_unuseful(Addr line_addr);
Addr decoupled_fe_get_last_cl_unuseful();
// FTQ API
Decoupled_FE* decoupled_fe_new_ftq_iter(uns proc_id, uns bp_id, uns* ftq_idx);
/* Returns the Op at current iterator position or NULL if FTQ is empty or the end of FTQ was reached
   if end_of_ft is true the Op is the last one in a fetch target (cache-line boundary of taken branch)*/
Op* decoupled_fe_ftq_iter_get(Decoupled_FE* dfe, uns iter_idx, bool *end_of_ft);
/* Increments iterator and returns the Op at iterator position or NULL if FTQ is empty or the end of FTQ was reached
   if end_of_ft is true the Op is the last one in a fetch target (cache-line boundary of taken branch)*/
Op* decoupled_fe_ftq_iter_get_next(Decoupled_FE* dfe, uns iter_idx, bool *end_of_ft);
/* Returns iter flattened offset from the start of the FTQ, this offset gets incremented
   by advancing the iter and decremented by the icache consuming FTQ entries,
   and reset by flushes */
uint64_t decoupled_fe_ftq_iter_offset(Decoupled_FE* dfe, uns iter_idx);
/* Returns iter ft offset from the start of the FTQ, this offset gets incremented
   by advancing the iter and decremented by the icache consuming FTQ entries,
   and reset by flushes */
uint64_t decoupled_fe_ftq_iter_ft_offset(Decoupled_FE* dfe, uns iter_idx);
uint64_t decoupled_fe_ftq_num_ops(Decoupled_FE* dfe);
uint64_t decoupled_fe_ftq_num_fts(Decoupled_FE* dfe);
void decoupled_fe_set_ftq_num(uint64_t ftq_ft_num);
uint64_t decoupled_fe_get_ftq_num();

#endif
