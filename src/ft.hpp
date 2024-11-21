#ifndef __FT_HPP_H__
#define __FT_HPP_H__

#include "decoupled_frontend.h"
#include <vector>

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
  Flag get_prefetch() { return prefetch; }
  void set_prefetch(Flag _prefetch) { prefetch = _prefetch; }

private:
  uns proc_id;
  // indicate the next op index to read by the consumer (icache or uop)
  uint64_t op_pos;
  FT_Info ft_info;
  std::vector<Op*> ops;
  Flag prefetch;

  friend class Decoupled_FE;
  friend class MP;
};

#endif
