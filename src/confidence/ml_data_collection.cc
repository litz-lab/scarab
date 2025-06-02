#include "confidence/ml_data_collection.hpp"

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_CONF, ##args)

MLDataCollectionStat::MLDataCollectionStat(uns _proc_id, MLDataCollection* _conf_mech) : ConfMechStatBase(_proc_id) {
  conf_mech = _conf_mech;
}

void MLDataCollection::per_cycle_update(Conf_Off_Path_Reason& new_reason) {
  DEBUG(proc_id, "Updating MLDataCollection for cycle %llu, btb_miss_rate: %f\n", cycle_count, btb_miss_rate);
  if (cycle_count % CONF_BTB_MISS_SAMPLE_RATE == 0) {
    btb_miss_rate = (double)cnt_btb_miss / (double)CONF_BTB_MISS_SAMPLE_RATE;
    cnt_btb_miss = 0;
  }
  if (cycle_count % CONF_IBTB_MISS_SAMPLE_RATE == 0) {
    ibtb_miss_rate = (double)cnt_ibtb_miss / (double)CONF_IBTB_MISS_SAMPLE_RATE;
    cnt_ibtb_miss = 0;
  }
  if (cycle_count % CONF_MISFETCH_SAMPLE_RATE == 0) {
    misfetch_rate = (double)cnt_misfetch / (double)CONF_MISFETCH_SAMPLE_RATE;
    cnt_misfetch = 0;
  }
  if (cycle_count % CONF_MISPRED_SAMPLE_RATE == 0) {
    mispred_rate = (double)cnt_mispred / (double)CONF_MISPRED_SAMPLE_RATE;
    cnt_mispred = 0;
  }
}

void MLDataCollection::recover(Op* op, std::deque<FT>& ftq) {
  DEBUG(proc_id, "Recovering MLDataCollection for off-path reason %d\n", op->oracle_info.off_path_reason);
  switch (op->oracle_info.off_path_reason) {
    case REASON_BTB_MISS:
      cnt_btb_miss++;
      last_btb_recover_cycle = cycle_count;
      break;
    case REASON_BTB_MISS_MISPRED:
      cnt_btb_miss++;
      cnt_mispred++;
      last_btb_recover_cycle = cycle_count;
      last_mispred_recover_cycle = cycle_count;
      break;
    case REASON_MISPRED:
      cnt_mispred++;
      last_mispred_recover_cycle = cycle_count;
      break;
    case REASON_MISFETCH:
      cnt_misfetch++;
      last_misfetch_recover_cycle = cycle_count;
      break;
  }
}

void MLDataCollectionStat::update(Op* op, Conf_Off_Path_Reason reason, bool last_in_ft, FT& pushed_ft) {
  ConfMechStatBase::update(op, reason, last_in_ft, pushed_ft);
  if (last_in_ft) {
    DEBUG(proc_id, "Last op in FT, cycle %llu, btb_miss_rate: %f\n", cycle_count, conf_mech->btb_miss_rate);
    set_ft_info(pushed_ft.get_ft_info());
  }
}

void MLDataCollectionStat::set_ft_info(FT_Info& ft_info) {
  // op ft_info isn't a reference. this doesn't acutually update the
  DEBUG(proc_id,
        "Setting FT info for cycle %llu, cycles_since_btb_rec: %llu, cycles_since_ibtb_rec: %llu, "
        "cycles_since_misfetch_rec: %llu, cycles_since_mispred_rec: %llu\n",
        cycle_count, cycle_count - conf_mech->last_btb_recover_cycle, cycle_count - conf_mech->last_ibtb_recover_cycle,
        cycle_count - conf_mech->last_misfetch_recover_cycle, cycle_count - conf_mech->last_mispred_recover_cycle);
  DEBUG(proc_id,
        "Setting FT info for cycle %llu, btb_miss_rate: %f, ibtb_miss_rate: %f, misfetch_rate: %f, mispred_rate: %f\n",
        cycle_count, conf_mech->btb_miss_rate, conf_mech->ibtb_miss_rate, conf_mech->misfetch_rate,
        conf_mech->mispred_rate);
  ft_info.dynamic_info.ml_data_info->dfe_cycle = cycle_count;
  ft_info.dynamic_info.ml_data_info->cycles_since_btb_rec = cycle_count - conf_mech->last_btb_recover_cycle;
  ft_info.dynamic_info.ml_data_info->cycles_since_ibtb_rec = cycle_count - conf_mech->last_ibtb_recover_cycle;
  ft_info.dynamic_info.ml_data_info->cycles_since_misfetch_rec = cycle_count - conf_mech->last_misfetch_recover_cycle;
  ft_info.dynamic_info.ml_data_info->cycles_since_mispred_rec = cycle_count - conf_mech->last_mispred_recover_cycle;
  ft_info.dynamic_info.ml_data_info->btb_miss_rate = conf_mech->btb_miss_rate;
  ft_info.dynamic_info.ml_data_info->ibtb_miss_rate = conf_mech->btb_miss_rate;
  ft_info.dynamic_info.ml_data_info->misfetch_rate = conf_mech->misfetch_rate;
  ft_info.dynamic_info.ml_data_info->mispred_rate = conf_mech->mispred_rate;
}

void MLDataCollectionStat::log_ft_data(FT_Info& ft_info, std::vector<Op*>& ops) {
  // ASSERT(set_proc_id, iter->op_pos == df_ftq->at(iter->ft_pos).ops.size() - 1);
  Addr start = ft_info.static_info.start;
  Addr length = ft_info.static_info.length;
  FT_Ended_By ended_by = ft_info.dynamic_info.ended_by;
  Counter dfe_cycle = ft_info.dynamic_info.ml_data_info->dfe_cycle;
  Counter icache_cycle = cycle_count;
  Counter cycles_since_btb_rec = ft_info.dynamic_info.ml_data_info->cycles_since_btb_rec;
  Counter cycles_since_ibtb_rec = ft_info.dynamic_info.ml_data_info->cycles_since_ibtb_rec;
  Counter cycles_since_misfetch_rec = ft_info.dynamic_info.ml_data_info->cycles_since_misfetch_rec;
  Counter cycles_since_mispred_rec = ft_info.dynamic_info.ml_data_info->cycles_since_mispred_rec;
  double btb_miss_rate = ft_info.dynamic_info.ml_data_info->btb_miss_rate;
  double ibtb_miss_rate = ft_info.dynamic_info.ml_data_info->ibtb_miss_rate;
  double misfetch_rate = ft_info.dynamic_info.ml_data_info->misfetch_rate;
  double mispred_rate = ft_info.dynamic_info.ml_data_info->mispred_rate;

  DEBUG(proc_id, "Logging FT data: btb_miss_rate: %f, ibtb_miss_rate: %f, misfetch_rate: %f, mispred_rate: %f\n",
        btb_miss_rate, ibtb_miss_rate, misfetch_rate, mispred_rate);
  DEBUG(proc_id, "Logging FT data: cnt_btb_miss: %lld, cnt_ibtb_miss: %lld, cnt_misfetch: %lld, cnt_mispred: %lld\n",
        conf_mech->cnt_btb_miss, conf_mech->cnt_ibtb_miss, conf_mech->cnt_misfetch, conf_mech->cnt_mispred);

  // Counter recover_cycle = 0;
  int op_num = 0;
  int cf_mask = 0;
  // enum tage_component {TAGE_BASE, TAGE_SHORT, TAGE_LONG, TAGE_LOOP, TAGE_SC, NOT_TAGE};
  bool tage_base = false;
  bool tage_short = false;
  bool tage_long = false;
  bool tage_loop = false;
  bool tage_sc = false;

  int off_path_reason = 0;
  int op_off_path = 0;

  for (auto op : ops) {
    // should only be non-zero for one op in the FT if any
    off_path_reason += (int)op->oracle_info.off_path_reason;
    // get the mask of ops that can be identified as CF
    cf_mask |= ((1 & (op->table_info->cf_type && !op->oracle_info.btb_miss)) << op_num);
    if (op->off_path) {
      op_off_path = 1;
    }
    switch (op->tage_comp) {
      case 0:
        tage_base = true;
        break;
      case 1:
        tage_short = true;
        break;
      case 2:
        tage_long = true;
        break;
      case 3:
        tage_loop = true;
        break;
      case 4:
        tage_sc = true;
        break;
    }
  }

  // add entry to vector of data
  ml_data.emplace_back(ml_data_entry(start, length, ended_by, dfe_cycle, icache_cycle, cycles_since_btb_rec,
                                     cycles_since_ibtb_rec, cycles_since_misfetch_rec, cycles_since_mispred_rec,
                                     btb_miss_rate, ibtb_miss_rate, misfetch_rate, mispred_rate, cf_mask, tage_base,
                                     tage_short, tage_long, tage_loop, tage_sc, off_path_reason, op_off_path));
}

void MLDataCollectionStat::print_data() {
  FILE* fp = fopen("ft_training_data.csv", "w");
  Addr ft_addr;
  Addr ft_length;
  FT_Ended_By ended_by;
  Counter dfe_cycle;
  Counter icache_cycle;
  Counter cycles_since_btb_rec;
  Counter cycles_since_ibtb_rec;
  Counter cycles_since_misfetch_rec;
  Counter cycles_since_mispred_rec;
  double btb_miss_rate;
  double ibtb_miss_rate;
  double misfetch_rate;
  double mispred_rate;
  int cf_mask;
  bool tage_comp_base;
  bool tage_comp_short;
  bool tage_comp_long;
  bool tage_comp_loop;
  bool tage_comp_sc;
  int off_path_reason;
  int op_off_path;

  fprintf(fp,
          "ft_start_addr,\
ft_length,\
ft_ended_by,\
dfe_cycle,\
icache_cycle,\
cycles_since_btb_rec,\
cycles_since_ibtb_rec,\
cycles_since_misfetch_rec,\
cycles_since_mispred_rec,\
btb_miss_rate,\
ibtb_miss_rate,\
misfetch_rate,\
mispred_rate,\
cf_mask,\
tage_comp_base,\
tage_comp_short,\
tage_comp_long,\
tage_comp_loop,\
tage_comp_sc,\
off_path_reason,\
off_path\n");
  for (const ml_data_entry& line : ml_data) {
    ft_addr = std::get<0>(line);
    ft_length = std::get<1>(line);
    ended_by = std::get<2>(line);
    dfe_cycle = std::get<3>(line);
    icache_cycle = std::get<4>(line);
    cycles_since_btb_rec = std::get<5>(line);
    cycles_since_ibtb_rec = std::get<6>(line);
    cycles_since_misfetch_rec = std::get<7>(line);
    cycles_since_mispred_rec = std::get<8>(line);
    btb_miss_rate = std::get<9>(line);
    ibtb_miss_rate = std::get<10>(line);
    misfetch_rate = std::get<11>(line);
    mispred_rate = std::get<12>(line);
    cf_mask = std::get<13>(line);
    tage_comp_base = std::get<14>(line);
    tage_comp_short = std::get<15>(line);
    tage_comp_long = std::get<16>(line);
    tage_comp_loop = std::get<17>(line);
    tage_comp_sc = std::get<18>(line);
    off_path_reason = std::get<19>(line);
    op_off_path = std::get<20>(line);
    fprintf(fp, "%llu,%llu,%d,%llu,%llu,%llu,%llu,%llu,%llu,%f,%f,%f,%f,%d,%d,%d,%d,%d,%d,%d,%d", ft_addr, ft_length,
            ended_by, dfe_cycle, icache_cycle, cycles_since_btb_rec, cycles_since_ibtb_rec, cycles_since_misfetch_rec,
            cycles_since_mispred_rec, btb_miss_rate, ibtb_miss_rate, misfetch_rate, mispred_rate, cf_mask,
            tage_comp_base, tage_comp_short, tage_comp_long, tage_comp_loop, tage_comp_sc, off_path_reason,
            op_off_path);
    fprintf(fp, "\n");
  }
  fclose(fp);
}