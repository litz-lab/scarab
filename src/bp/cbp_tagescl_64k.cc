#include "cbp_tagescl_64k.h"

TAGE64K::TAGE64K(void) {
  memset(SizeTable, 0, sizeof(SizeTable));
  memset(NOSKIP, 0, sizeof(NOSKIP));
  memset(m, 0, sizeof(m));
  memset(TB, 0, sizeof(TB));
  memset(logg, 0, sizeof(logg));
  memset(noskip_index, 0, sizeof(noskip_index));
  Pstate.init();
  Sstate.init();
  memset(Bias, 0, sizeof(Bias));
  memset(BiasSK, 0, sizeof(BiasSK));
  memset(BiasBank, 0, sizeof(BiasBank));
  memset(LGEHL, 0, sizeof(LGEHL));
  memset(SGEHL, 0, sizeof(SGEHL));
  memset(TGEHL, 0, sizeof(TGEHL));
#ifdef IMLI
  memset(IMGEHL, 0, sizeof(IMGEHL));
  memset(IGEHL, 0, sizeof(IGEHL));
  memset(IMHIST, 0, sizeof(IMHIST));
  IMLIcount = 0;
#endif
  memset(L_shist, 0, sizeof(L_shist));
  memset(S_slhist, 0, sizeof(S_slhist));
  memset(T_slhist, 0, sizeof(T_slhist));
  updatethreshold = 0;
  memset(Pupdatethreshold, 0, sizeof(Pupdatethreshold));
  memset(WL, 0, sizeof(WL));
  memset(WS, 0, sizeof(WS));
  memset(WT, 0, sizeof(WT));
  memset(WI, 0, sizeof(WI));
  memset(WIM, 0, sizeof(WIM));
  memset(WB, 0, sizeof(WB));
  FirstH = 0;
  SecondH = 0;
  WITHLOOP = 0;
  LIB = 0;
  LI = 0;
  LTAG = 0;
  memset(use_alt_on_na, 0, sizeof(use_alt_on_na));
  TICK = 0;
  branch_id = 0;
  Seed = 0;
  tage_component = 0;
  tage_component_inter = 0;
  tage_component_tage = 0;
  tage_component_alt = 0;
  reinit();
  // #ifdef PRINTSIZE
  // predictorsize ();
  // #endif
}

uns8 TAGE64K::IsFull(void) {
  return 0;
}

void TAGE64K::reinit() {
  m[1] = MINHIST;
  m[NHIST / 2] = MAXHIST;
  for (int i = 2; i <= NHIST / 2; i++) {
    m[i] = (int)(((double)MINHIST *
                  pow((double)(MAXHIST) / (double)MINHIST, (double)(i - 1) / (double)(((NHIST / 2) - 1)))) +
                 0.5);
    //      fprintf(stderr, "(%d %d)", m[i],i);
  }
  for (int i = 1; i <= NHIST; i++) {
    NOSKIP[i] = ((i - 1) & 1) || ((i >= BORNINFASSOC) & (i < BORNSUPASSOC));
  }

  // just eliminate some extra tables (very very marginal)
  // Author removed4 entries manually, set MANUAL_OMIT 4 to specify the entries to be omitted
  NOSKIP[4] = 0;
  NOSKIP[NHIST - 2] = 0;
  NOSKIP[8] = 0;
  NOSKIP[NHIST - 6] = 0;

  // Since the entry of global Pstate and the entry of checkpoint have different size,
  // we need to map the index of NOSKIP to the index of Pstate and checkpoint
  int idx_n = 0;
  for (int i = 0; i <= NHIST; i++) {
    if (NOSKIP[i])
      noskip_index[i] = idx_n++;
    else
      noskip_index[i] = -1;
  }
  ASSERTM(0, idx_n == NOSKIPCNT, "incorrect idx_n: %d", idx_n);

  for (int i = NHIST; i > 1; i--) {
    m[i] = m[(i + 1) / 2];
  }
  for (int i = 1; i <= NHIST; i++) {
    TB[i] = TBITS + 4 * (i >= BORN);
    logg[i] = LOGG;
  }

  gtable[1] = new cbp64_gentry[NBANKLOW * (1 << LOGG)];
  SizeTable[1] = NBANKLOW * (1 << LOGG);

  gtable[BORN] = new cbp64_gentry[NBANKHIGH * (1 << LOGG)];
  SizeTable[BORN] = NBANKHIGH * (1 << LOGG);

  for (int i = BORN + 1; i <= NHIST; i++)
    gtable[i] = gtable[BORN];
  for (int i = 2; i <= BORN - 1; i++)
    gtable[i] = gtable[1];
  btable = new cbp64_bentry[1 << LOGB];

  for (int i = 1; i <= NHIST; i++) {
    Sstate.ch_i[i].init(m[i], (logg[i]));
    Sstate.ch_t[0][i].init(Sstate.ch_i[i].OLENGTH, TB[i]);
    Sstate.ch_t[1][i].init(Sstate.ch_i[i].OLENGTH, TB[i] - 1);
  }
  if (TAGESCL64KB_LOOP) {
    Pstate.LVALID = false;
    WITHLOOP = -1;
  }

  TICK = 0;
  Sstate.phist = 0;
  Seed = 0;

  for (int i = 0; i < HISTBUFFERLENGTH; i++)
    Sstate.ghist[0] = 0;
  Sstate.ptghist = 0;
  updatethreshold = 35 << 3;

  for (int i = 0; i < (1 << LOGSIZEUP); i++)
    Pupdatethreshold[i] = 0;
  for (int i = 0; i < GNB; i++)
    Sstate.GGEHL[i] = &GGEHLA[i][0];
  for (int i = 0; i < LNB; i++)
    LGEHL[i] = &LGEHLA[i][0];

  for (int i = 0; i < GNB; i++)
    for (int j = 0; j < ((1 << LOGGNB) - 1); j++) {
      if (!(j & 1)) {
        Sstate.GGEHL[i][j] = -1;
      }
    }
  for (int i = 0; i < LNB; i++)
    for (int j = 0; j < ((1 << LOGLNB) - 1); j++) {
      if (!(j & 1)) {
        LGEHL[i][j] = -1;
      }
    }

  for (int i = 0; i < SNB; i++)
    SGEHL[i] = &SGEHLA[i][0];
  for (int i = 0; i < TNB; i++)
    TGEHL[i] = &TGEHLA[i][0];
  for (int i = 0; i < PNB; i++)
    Sstate.PGEHL[i] = &PGEHLA[i][0];
#ifdef IMLI
#ifdef IMLIOH
  for (int i = 0; i < FNB; i++)
    FGEHL[i] = &FGEHLA[i][0];

  for (int i = 0; i < FNB; i++)
    for (int j = 0; j < ((1 << LOGFNB) - 1); j++) {
      if (!(j & 1)) {
        FGEHL[i][j] = -1;
      }
    }
#endif
  for (int i = 0; i < INB; i++)
    IGEHL[i] = &IGEHLA[i][0];
  for (int i = 0; i < INB; i++)
    for (int j = 0; j < ((1 << LOGINB) - 1); j++) {
      if (!(j & 1)) {
        IGEHL[i][j] = -1;
      }
    }
  for (int i = 0; i < IMNB; i++)
    IMGEHL[i] = &IMGEHLA[i][0];
  for (int i = 0; i < IMNB; i++)
    for (int j = 0; j < ((1 << LOGIMNB) - 1); j++) {
      if (!(j & 1)) {
        IMGEHL[i][j] = -1;
      }
    }

#endif
  for (int i = 0; i < SNB; i++)
    for (int j = 0; j < ((1 << LOGSNB) - 1); j++) {
      if (!(j & 1)) {
        SGEHL[i][j] = -1;
      }
    }
  for (int i = 0; i < TNB; i++)
    for (int j = 0; j < ((1 << LOGTNB) - 1); j++) {
      if (!(j & 1)) {
        TGEHL[i][j] = -1;
      }
    }
  for (int i = 0; i < PNB; i++)
    for (int j = 0; j < ((1 << LOGPNB) - 1); j++) {
      if (!(j & 1)) {
        Sstate.PGEHL[i][j] = -1;
      }
    }

  for (int i = 0; i < (1 << LOGB); i++) {
    btable[i].pred = 0;
    btable[i].hyst = 1;
  }

  for (int j = 0; j < (1 << LOGBIAS); j++) {
    switch (j & 3) {
      case 0:
        BiasSK[j] = -8;
        break;
      case 1:
        BiasSK[j] = 7;
        break;
      case 2:
        BiasSK[j] = -32;

        break;
      case 3:
        BiasSK[j] = 31;
        break;
    }
  }
  for (int j = 0; j < (1 << LOGBIAS); j++) {
    switch (j & 3) {
      case 0:
        Bias[j] = -32;

        break;
      case 1:
        Bias[j] = 31;
        break;
      case 2:
        Bias[j] = -1;
        break;
      case 3:
        Bias[j] = 0;
        break;
    }
  }
  for (int j = 0; j < (1 << LOGBIAS); j++) {
    switch (j & 3) {
      case 0:
        BiasBank[j] = -32;

        break;
      case 1:
        BiasBank[j] = 31;
        break;
      case 2:
        BiasBank[j] = -1;
        break;
      case 3:
        BiasBank[j] = 0;
        break;
    }
  }
  for (int i = 0; i < SIZEUSEALT; i++) {
    use_alt_on_na[i] = 0;
  }
  for (int i = 0; i < (1 << LOGSIZEUPS); i++) {
    Sstate.WG[i] = 7;
    Sstate.WP[i] = 7;
    WL[i] = 7;
    WS[i] = 7;
    WT[i] = 7;
    WI[i] = 7;
    WB[i] = 4;
  }
  for (int i = 0; i < NLOCAL; i++) {
    L_shist[i] = 0;
  }
  for (int i = 0; i < NSECLOCAL; i++) {
    S_slhist[i] = 0;
  }
  TICK = 0;
  Sstate.GHIST = 0;
  Pstate.on_path_phist = 0;
  Pstate.on_path_ptghist = 0;
  branch_id = 0;

  tage_component = TAGE_BASE;
  tage_component_alt = TAGE_BASE;
}

// the index functions for the tagged tables uses path history as in the OGEHL predictor
// F serves to mix path history: not very important impact

int TAGE64K::F(long long A, int size, int bank) {
  int A1, A2;
  A = A & ((1 << size) - 1);
  A1 = (A & ((1 << logg[bank]) - 1));
  A2 = (A >> logg[bank]);

  if (bank < logg[bank])
    A2 = ((A2 << bank) & ((1 << logg[bank]) - 1)) + (A2 >> (logg[bank] - bank));
  A = A1 ^ A2;
  if (bank < logg[bank])
    A = ((A << bank) & ((1 << logg[bank]) - 1)) + (A >> (logg[bank] - bank));
  return (A);
}

// gindex computes a full hash of PC, ghist and phist
int TAGE64K::gindex(unsigned int PC, int bank, long long hist, cbp64_folded_history* ch_i) {
  int index;
  int M = (m[bank] > PHISTWIDTH) ? PHISTWIDTH : m[bank];
  index = PC ^ (PC >> (abs(logg[bank] - bank) + 1)) ^ ch_i[bank].comp ^ F(hist, M, bank);

  return (index & ((1 << (logg[bank])) - 1));
}

//  tag computation
uint16_t TAGE64K::gtag(unsigned int PC, int bank, cbp64_folded_history* ch0, cbp64_folded_history* ch1) {
  int tag = (PC) ^ ch0[bank].comp ^ (ch1[bank].comp << 1);
  return (tag & ((1 << (TB[bank])) - 1));
}

// up-down saturating counter
void TAGE64K::ctrupdate(int8_t& ctr, bool taken, int nbits) {
  if (taken) {
    if (ctr < ((1 << (nbits - 1)) - 1))
      ctr++;
  } else {
    if (ctr > -(1 << (nbits - 1)))
      ctr--;
  }
}

#define GET_BI(PC) ((PC ^ (PC >> 2)) & ((1 << LOGB) - 1))
bool TAGE64K::getbim(UINT64 PC) {
  int BI = GET_BI(PC);
  int8_t BIM = (btable[BI].pred << 1) + (btable[BI >> HYSTSHIFT].hyst);

  Pstate.HighConf = (BIM == 0) || (BIM == 3);
  Pstate.LowConf = !Pstate.HighConf;
  Pstate.AltConf = Pstate.HighConf;
  Pstate.MedConf = false;
  return (btable[BI].pred > 0);
}

void TAGE64K::baseupdate(bool Taken, UINT64 PC) {
  int BI = GET_BI(PC);
  int8_t BIM = (btable[BI].pred << 1) + (btable[BI >> HYSTSHIFT].hyst);
  int inter = BIM;
  if (Taken) {
    if (inter < 3)
      inter += 1;
  } else if (inter > 0)
    inter--;
  btable[BI].pred = inter >> 1;
  btable[BI >> HYSTSHIFT].hyst = (inter & 1);
};

// just a simple pseudo random number generator: use available information
//  to allocate entries  in the loop predictor
int TAGE64K::MYRANDOM(long long on_path_phist, int on_path_ptghist, bool off_path) {
  if (RANDOM_DETERMINISTIC && off_path)
    return Seed;

  long long phist = (RANDOM_DETERMINISTIC) ? on_path_phist : Sstate.phist;
  int ptghist = (RANDOM_DETERMINISTIC) ? on_path_ptghist : Sstate.ptghist;

  Seed++;
  Seed ^= phist;
  Seed = (Seed >> 21) + (Seed << 11);
  Seed ^= ptghist;
  Seed = (Seed >> 10) + (Seed << 22);
  return (Seed);
};

void TAGE64K::SavePredictorStates(Counter key) {
  if (RANDOM_DETERMINISTIC) {
    Pstate.on_path_phist = Sstate.phist;
    Pstate.on_path_ptghist = Sstate.ptghist;
  }
  if (SPEC_LEVEL == BP_PRED_ON)
    return;

  auto& key_index = predictor_states.get<0>();  // Get the key index
  auto it = key_index.find(key);
  assert(it == key_index.end());

  PredictorStates state(true);
  // LOOP
  if (TAGESCL64KB_LOOP) {
    state.LHIT = Pstate.LHIT;
    state.LVALID = Pstate.LVALID;
    state.predloop = Pstate.predloop;
  }
  // SC
  if (TAGESCL64KB_SC) {
    state.THRES = Pstate.THRES;
    state.LSUM = Pstate.LSUM;
  }
  // TAGE
  if (TAGESCL64KB_ALT) {
    state.alttaken = Pstate.alttaken;
    state.AltConf = Pstate.AltConf;
    state.AltBank = Pstate.AltBank;
    assert(NOSKIP[state.AltBank] || state.AltBank == 0);
  }
  state.tage_pred = Pstate.tage_pred;
  state.pred_inter = Pstate.pred_inter;
  state.pred_taken = Pstate.pred_taken;
  state.LongestMatchPred = Pstate.LongestMatchPred;
  state.HighConf = Pstate.HighConf;
  state.MedConf = Pstate.MedConf;
  state.LowConf = Pstate.LowConf;
  state.HitBank = Pstate.HitBank;
  if (RANDOM_DETERMINISTIC) {
    state.on_path_phist = Pstate.on_path_phist;
    state.on_path_ptghist = Pstate.on_path_ptghist;
  }
  assert(NOSKIP[state.HitBank] || state.HitBank == 0);  // HitBank should be valid or 0
  int8_t j = 0;
  for (int i = 0; i <= NHIST; i++) {
    if (NOSKIP[i]) {
      state.GI[j] = Pstate.GI[i];
      state.GTAG[j] = Pstate.GTAG[i];
      assert(state.GI[j] != -1);
      assert(noskip_index[i] != -1);
      j++;
    }
  }
  predictor_states.insert(PredictorEntry(key, std::move(state)));
  it = key_index.find(key);
  ASSERTM(0, it != key_index.end(), "it != key_index.end()");
}

Counter TAGE64K::KeyGeneration() {
  return ++branch_id;
}

void TAGE64K::RestorePredictorstates(Counter key) {
  auto& key_pindex = predictor_states.get<0>();
  auto pit = key_pindex.find(key);
  assert(pit != key_pindex.end());
  // LOOP
  if (TAGESCL64KB_LOOP) {
    Pstate.LHIT = pit->state.LHIT;
    Pstate.LVALID = pit->state.LVALID;
    Pstate.predloop = pit->state.predloop;
  }
  // SC
  if (TAGESCL64KB_SC) {
    Pstate.THRES = pit->state.THRES;
    Pstate.LSUM = pit->state.LSUM;
  }
  // TAGE
  Pstate.tage_pred = pit->state.tage_pred;
  Pstate.pred_inter = pit->state.pred_inter;
  Pstate.pred_taken = pit->state.pred_taken;
  Pstate.LongestMatchPred = pit->state.LongestMatchPred;
  if (TAGESCL64KB_ALT) {
    Pstate.alttaken = pit->state.alttaken;
    Pstate.AltConf = pit->state.AltConf;
    Pstate.AltBank = pit->state.AltBank;
  }
  Pstate.HitBank = pit->state.HitBank;
  Pstate.HighConf = pit->state.HighConf;
  Pstate.MedConf = pit->state.MedConf;
  Pstate.LowConf = pit->state.LowConf;
  // store only elements with NOSKIP
  int j = 0;
  for (int i = 0; i <= NHIST; i++) {
    if (NOSKIP[i]) {
      assert(noskip_index[i] >= 0);
      Pstate.GI[i] = pit->state.GI[j];
      Pstate.GTAG[i] = pit->state.GTAG[j];
      j++;
    }
  }
}

void TAGE64K::TakeCheckpoint(Counter key) {
  auto& key_index = checkpoints.get<0>();  // Get the key index
  auto it = key_index.find(key);
  assert(it == key_index.end());
  long unsigned int org_size = checkpoints.size();
  Checkpoint state;
  // Copy scalar values
  state.ptghist = Sstate.ptghist;
  state.phist = Sstate.phist;
  // Copy folded histories - use memcpy for better performance
  std::copy(std::begin(Sstate.ch_i), std::end(Sstate.ch_i), std::begin(state.ch_i));
  std::copy(std::begin(Sstate.ch_t[0]), std::end(Sstate.ch_t[0]), std::begin(state.ch_t[0]));
  std::copy(std::begin(Sstate.ch_t[1]), std::end(Sstate.ch_t[1]), std::begin(state.ch_t[1]));
  if (TAGESCL64KB_SC) {
    state.GHIST = Sstate.GHIST;
    // Copy GEHL arrays
    for (int i = 0; i < GNB; ++i) {
      std::copy(Sstate.GGEHL[i], Sstate.GGEHL[i] + (1 << LOGGNB), state.GGEHL[i]);
    }
    for (int i = 0; i < PNB; ++i) {
      std::copy(Sstate.PGEHL[i], Sstate.PGEHL[i] + (1 << LOGPNB), state.PGEHL[i]);
    }
    // Copy weight tables
    std::copy(Sstate.WG, Sstate.WG + (1 << LOGSIZEUPS), state.WG);
    std::copy(Sstate.WP, Sstate.WP + (1 << LOGSIZEUPS), state.WP);
  }
  if (TAGESCL64KB_LOOP)
    std::copy(Sstate.ltable, Sstate.ltable + (1 << LOGL), state.ltable);
  checkpoints.insert(CheckpointEntry(key, state));
  assert(checkpoints.size() == (org_size + 1));
}

void TAGE64K::VerifyCheckpoint(Counter key) {
  auto& key_index = checkpoints.get<0>();  // Get the key index
  auto it = key_index.find(key);
  assert(it != key_index.end());
  // checkpoint states
  CompareCheckpoint(it->state);
  it = key_index.upper_bound(key);
  if (it != key_index.end())
    key_index.erase(it, key_index.end());
}

void TAGE64K::VerifyPredictorStates(Counter key) {
  auto& key_pindex = predictor_states.get<0>();
  auto pit = key_pindex.find(key);
  assert(pit != key_pindex.end());
  auto& mutable_state = const_cast<PredictorStates&>(pit->state);
  // predictor states
  ComparePredictor(mutable_state);
  pit = key_pindex.upper_bound(key);
  if (pit != key_pindex.end())
    key_pindex.erase(pit, key_pindex.end());
}

void TAGE64K::RestoreStates(Counter key, UINT64 PC, OpType optype, Flag is_conditional, Flag dir, UINT64 target) {
  // recover bp states
  if (SPEC_LEVEL < BP_PRED_ON_SPEC_UPDATE_S_ONOFF_N_ON)
    VerifyCheckpoint(key);
  else {
    RestoreCheckpoint(key);
    if (SPEC_LEVEL < BP_PRED_ONOFF_SPEC_UPDATE_S_ONOFF_UPDATE_N_ON)
      return;
    if (is_conditional)
      SpecUpdateAtCond(PC, dir, false);
    SpecUpdate(PC, optype, dir, target);
  }
}

void TAGE64K::RestoreCheckpoint(Counter key) {
  auto& key_index = checkpoints.get<0>();
  auto it = key_index.find(key);
  assert(it != key_index.end());  // Ensure the checkpoint exists
  // Restore global history
  Sstate.phist = it->state.phist;
  Sstate.ptghist = it->state.ptghist;
  // Restore folded histories
  std::copy(std::begin(it->state.ch_i), std::end(it->state.ch_i), std::begin(Sstate.ch_i));
  std::copy(std::begin(it->state.ch_t[0]), std::end(it->state.ch_t[0]), std::begin(Sstate.ch_t[0]));
  std::copy(std::begin(it->state.ch_t[1]), std::end(it->state.ch_t[1]), std::begin(Sstate.ch_t[1]));
  if (TAGESCL64KB_SC) {
    Sstate.GHIST = it->state.GHIST;
    // Restore GEHL arrays
    for (int i = 0; i < GNB; ++i) {
      std::copy(it->state.GGEHL[i], it->state.GGEHL[i] + (1 << LOGGNB), Sstate.GGEHL[i]);
    }
    for (int i = 0; i < PNB; ++i) {
      std::copy(it->state.PGEHL[i], it->state.PGEHL[i] + (1 << LOGPNB), Sstate.PGEHL[i]);
    }
    std::copy(it->state.WG, it->state.WG + (1 << LOGSIZEUPS), Sstate.WG);
    std::copy(it->state.WP, it->state.WP + (1 << LOGSIZEUPS), Sstate.WP);
  }
  if (TAGESCL64KB_LOOP)
    std::copy(it->state.ltable, it->state.ltable + (1 << LOGL), Sstate.ltable);
  // Restore predictor states
  RestorePredictorstates(key);
  auto& key_pindex = predictor_states.get<0>();
  auto pit = key_pindex.upper_bound(key);
  it = key_index.upper_bound(key);
  if (pit != key_pindex.end())
    key_pindex.erase(pit, key_pindex.end());
  if (it != key_index.end())
    key_index.erase(it, key_index.end());
}

void TAGE64K::RetireCheckpoint(Counter key) {
  auto& key_index = checkpoints.get<0>();
  auto& key_pindex = predictor_states.get<0>();
  auto it = key_index.find(key);
  auto pit = key_pindex.find(key);
  // assert(it != key_index.end()); // key can be not exist
  assert(pit != key_pindex.end());  // Ensure the checkpoint exists
  if (it != key_index.end())
    key_index.erase(it);
  if (pit != key_pindex.end())
    key_pindex.erase(pit);
}

void TAGE64K::UpdateAddr(UINT64 PC, long long path_history, cbp64_folded_history* index, cbp64_folded_history* tag0,
                         cbp64_folded_history* tag1) {
  for (int i = 1; i <= NHIST; i += 2) {
    Pstate.GI[i] = gindex(PC, i, path_history, index);
    Pstate.GTAG[i] = gtag(PC, i, tag0, tag1);
    Pstate.GTAG[i + 1] = Pstate.GTAG[i];
    Pstate.GI[i + 1] = Pstate.GI[i] ^ (Pstate.GTAG[i] & ((1 << LOGG) - 1));
  }
  int T = (PC ^ (path_history & ((1 << m[BORN]) - 1))) % NBANKHIGH;
  for (int i = BORN; i <= NHIST; i++)
    if (NOSKIP[i]) {
      Pstate.GI[i] += (T << LOGG);
      T++;
      T = T % NBANKHIGH;
    }
  T = (PC ^ (path_history & ((1 << m[1]) - 1))) % NBANKLOW;
  for (int i = 1; i <= BORN - 1; i++)
    if (NOSKIP[i]) {
      Pstate.GI[i] += (T << LOGG);
      T++;
      T = T % NBANKLOW;
    }
}

//  TAGE PREDICTION: same code at fetch or retire time but the index and tags must recomputed
void TAGE64K::Tagepred(UINT64 PC) {
  Pstate.HitBank = 0;
  Pstate.AltBank = 0;
  UpdateAddr(PC, Sstate.phist, Sstate.ch_i, Sstate.ch_t[0], Sstate.ch_t[1]);
  Pstate.alttaken = getbim(PC);
  Pstate.tage_pred = Pstate.alttaken;
  Pstate.LongestMatchPred = Pstate.alttaken;
  tage_component_tage = TAGE_BASE;
  if (TAGESCL64KB_ALT) {
    tage_component_alt = TAGE_BASE;
  }

  // Look for the bank with longest matching history
  for (int i = NHIST; i > 0; i--) {
    if (NOSKIP[i])
      if (gtable[i][Pstate.GI[i]].tag == Pstate.GTAG[i]) {
        Pstate.HitBank = i;
        Pstate.LongestMatchPred = (gtable[Pstate.HitBank][Pstate.GI[Pstate.HitBank]].ctr >= 0);
        break;
      }
  }

  // Look for the alternate bank
  if (TAGESCL64KB_ALT) {
    for (int i = Pstate.HitBank - 1; i > 0; i--) {
      if (NOSKIP[i])
        if (gtable[i][Pstate.GI[i]].tag == Pstate.GTAG[i]) {
          Pstate.AltBank = i;
          break;
        }
    }
  }
  // computes the prediction and the alternate prediction

  if (Pstate.HitBank > 0) {
    if (TAGESCL64KB_ALT) {
      if (Pstate.AltBank > 0) {
        Pstate.alttaken = (gtable[Pstate.AltBank][Pstate.GI[Pstate.AltBank]].ctr >= 0);
        Pstate.AltConf = (abs(2 * gtable[Pstate.AltBank][Pstate.GI[Pstate.AltBank]].ctr + 1) > 1);
        tage_component_alt =
            (Pstate.AltBank >= BORN) ? TAGE_LONG : TAGE_SHORT;  // tag hit at the longer history length: TAGE_LONG
      } else
        Pstate.alttaken = getbim(PC);
      // if the entry is recognized as a newly allocated entry and
      // USE_ALT_ON_NA is positive  use the alternate prediction
      bool Huse_alt_on_na = (use_alt_on_na[INDUSEALT(Pstate)] >= 0);
      if (!Huse_alt_on_na || (abs(2 * gtable[Pstate.HitBank][Pstate.GI[Pstate.HitBank]].ctr + 1) > 1)) {
        Pstate.tage_pred = Pstate.LongestMatchPred;
        tage_component_tage = (Pstate.HitBank >= BORN) ? TAGE_LONG : TAGE_SHORT;
      } else {
        Pstate.tage_pred = Pstate.alttaken;
        tage_component_tage = tage_component_alt;
      }
    } else if (abs(2 * gtable[Pstate.HitBank][Pstate.GI[Pstate.HitBank]].ctr + 1) > 1) {
      Pstate.tage_pred = Pstate.LongestMatchPred;
      tage_component_tage = (Pstate.HitBank >= BORN) ? TAGE_LONG : TAGE_SHORT;
    }

    Pstate.HighConf = (abs(2 * gtable[Pstate.HitBank][Pstate.GI[Pstate.HitBank]].ctr + 1) >= (1 << CWIDTH) - 1);
    Pstate.LowConf = (abs(2 * gtable[Pstate.HitBank][Pstate.GI[Pstate.HitBank]].ctr + 1) == 1);
    Pstate.MedConf = (abs(2 * gtable[Pstate.HitBank][Pstate.GI[Pstate.HitBank]].ctr + 1) == 5);
  }
}

// compute the prediction
bool TAGE64K::GetPrediction(UINT64 PC, int* bp_confidence, Op* op) {
  // computes the TAGE table addresses and the partial tags

  Tagepred(PC);
  Pstate.pred_taken = Pstate.tage_pred;
  tage_component = tage_component_tage;

  if (!TAGESCL64KB_SC) {
    if (!op->off_path)
      STAT_EVENT(op->proc_id, TAGESCL_COMP_TAGE_BASE_CORRECT + op->oracle_info.mispred + tage_component * 2);
  }

  if (TAGESCL64KB_LOOP) {
    Pstate.predloop = getloop(PC);  // loop prediction
    Pstate.pred_taken = ((WITHLOOP >= 0) && (Pstate.LVALID)) ? Pstate.predloop : Pstate.pred_taken;
    tage_component = ((WITHLOOP >= 0) && (Pstate.LVALID))
                         ? TAGE_LOOP
                         : tage_component_tage;  // update STATUS to loop predictor or keep component level
  }

  Pstate.pred_inter = Pstate.pred_taken;
  tage_component_inter = tage_component;

  // Compute the SC prediction
  if (TAGESCL64KB_SC) {
    Pstate.LSUM = 0;

    // integrate BIAS prediction
    int8_t ctr = Bias[INDBIAS(Pstate)];

    Pstate.LSUM += (2 * ctr + 1);
    ctr = BiasSK[INDBIASSK(Pstate)];
    Pstate.LSUM += (2 * ctr + 1);
    ctr = BiasBank[INDBIASBANK(Pstate)];
    Pstate.LSUM += (2 * ctr + 1);
#ifdef VARTHRES
    Pstate.LSUM = (1 + (WB[INDUPDS] >= 0)) * Pstate.LSUM;
#endif
    // integrate the GEHL predictions
    Pstate.LSUM += Gpredict((PC << 1) + Pstate.pred_inter, Sstate.GHIST, Gm, Sstate.GGEHL, GNB, LOGGNB, Sstate.WG);
    Pstate.LSUM += Gpredict(PC, Sstate.phist, Pm, Sstate.PGEHL, PNB, LOGPNB, Sstate.WP);
#ifdef LOCALH
    Pstate.LSUM += Gpredict(PC, L_shist[INDLOCAL], Lm, LGEHL, LNB, LOGLNB, WL);
#ifdef LOCALS
    Pstate.LSUM += Gpredict(PC, S_slhist[INDSLOCAL], Sm, SGEHL, SNB, LOGSNB, WS);
#endif
#ifdef LOCALT
    Pstate.LSUM += Gpredict(PC, T_slhist[INDTLOCAL], Tm, TGEHL, TNB, LOGTNB, WT);
#endif
#endif

#ifdef IMLI
    Pstate.LSUM += Gpredict(PC, IMHIST[(IMLIcount)], IMm, IMGEHL, IMNB, LOGIMNB, WIM);
    Pstate.LSUM += Gpredict(PC, IMLIcount, Im, IGEHL, INB, LOGINB, WI);
#endif
    bool SCPRED = (Pstate.LSUM >= 0);
    // just  an heuristic if the respective contribution of component groups can be multiplied by 2 or not
    Pstate.THRES = (updatethreshold >> 3) + Pupdatethreshold[INDUPD]
#ifdef VARTHRES
                   + 12 * ((WB[INDUPDS] >= 0) + (Sstate.WP[INDUPDS] >= 0)
#ifdef LOCALH
                           + (WS[INDUPDS] >= 0) + (WT[INDUPDS] >= 0) + (WL[INDUPDS] >= 0)
#endif
                           + (Sstate.WG[INDUPDS] >= 0)
#ifdef IMLI
                           + (WI[INDUPDS] >= 0)
#endif
                          )
#endif
        ;

    // Minimal benefit in trying to avoid accuracy loss on low confidence SC prediction and  high/medium confidence on
    // TAGE
    //  but just uses 2 counters 0.3 % MPKI reduction
    if (Pstate.pred_inter != SCPRED) {
      // Choser uses TAGE confidence and |LSUM|
      Pstate.pred_taken = SCPRED;
      tage_component = TAGE_SC;
      if (Pstate.HighConf) {
        // if |LSUM| is 'much' smaller than threshold, final prediction is pred_inter(tage or loop)
        if ((abs(Pstate.LSUM) < Pstate.THRES / 4)) {
          Pstate.pred_taken = Pstate.pred_inter;
          tage_component = tage_component_inter;
        }
        // if |LSUM| is 'slightly' smaller than threshold(THRES / 4 <= |LSUM| < THRES / 2), select pred_inter when
        // SecondH
        // >= 0
        else if ((abs(Pstate.LSUM) < Pstate.THRES / 2)) {
          Pstate.pred_taken = (SecondH < 0) ? SCPRED : Pstate.pred_inter;
          tage_component = (SecondH < 0) ? TAGE_SC : tage_component_inter;
          if (Pstate.pred_taken == SCPRED && !op->off_path)
            STAT_EVENT(op->proc_id, TAGESCL_SC_SMALLLSUM + 2);  // SC from HighConf
        }
      }
      // if |LSUM| is 'much' smaller than threshold and FirstH < 0), select pred_inter
      if (Pstate.MedConf) {
        if ((abs(Pstate.LSUM) < Pstate.THRES / 4)) {
          Pstate.pred_taken = (FirstH < 0) ? SCPRED : Pstate.pred_inter;
          tage_component = (FirstH < 0) ? TAGE_SC : tage_component_inter;
          if (Pstate.pred_taken == SCPRED && !op->off_path)
            STAT_EVENT(op->proc_id, TAGESCL_SC_SMALLLSUM + 1);  // SC from MedConf
        }
      }
      if (!Pstate.MedConf && !Pstate.HighConf && Pstate.pred_taken == SCPRED && !op->off_path) {
        STAT_EVENT(op->proc_id, TAGESCL_SC_SMALLLSUM);  // exclusive SC not MedConf or HighConf
      }
    } else if (Pstate.pred_inter == SCPRED) {
      if (!op->off_path)
        STAT_EVENT(op->proc_id, TAGESCL_SC_SMALLLSUM + 3);
    }
  }

  if (!op->off_path) {
    STAT_EVENT(op->proc_id,
               TAGESCL_COMP_TAGE_BASE_CORRECT + (Pstate.pred_taken != op->oracle_info.dir) + tage_component * 2);
    if (op->oracle_info.btb_miss)
      STAT_EVENT(op->proc_id, TAGESCL_COMP_BTB_MISS_TAGE_BASE_CORRECT + (Pstate.pred_taken != op->oracle_info.dir) +
                                  tage_component * 2);
  }

  *bp_confidence = 0;
  if (Pstate.HighConf)
    *bp_confidence += 3;
  if (Pstate.MedConf)
    *bp_confidence += 2;
  if (Pstate.LowConf)
    *bp_confidence += 1;
  assert(*bp_confidence < 4);

  if ((Pstate.pred_taken != op->oracle_info.dir) && !op->off_path) {  // collect only for misprediction
    STAT_EVENT(op->proc_id, TAGESCL_CONF_0_PER_BASE_MISPREDICT + *bp_confidence + tage_component * 4);
  }
  return Pstate.pred_taken;
}

int TAGE64K::GetBrtypeFromOptype(OpType opType) {
  int brtype = 0;

  switch (opType) {
    case OPTYPE_RET_UNCOND:
    case OPTYPE_JMP_INDIRECT_UNCOND:
    case OPTYPE_JMP_INDIRECT_COND:
    case OPTYPE_CALL_INDIRECT_UNCOND:
    case OPTYPE_CALL_INDIRECT_COND:
    case OPTYPE_RET_COND:
      brtype = 2;
      break;
    case OPTYPE_JMP_DIRECT_COND:
    case OPTYPE_CALL_DIRECT_COND:
    case OPTYPE_JMP_DIRECT_UNCOND:
    case OPTYPE_CALL_DIRECT_UNCOND:
      brtype = 0;
      break;
    default:
      exit(1);
  }
  switch (opType) {
    case OPTYPE_JMP_DIRECT_COND:
    case OPTYPE_CALL_DIRECT_COND:
    case OPTYPE_JMP_INDIRECT_COND:
    case OPTYPE_CALL_INDIRECT_COND:
    case OPTYPE_RET_COND:
      brtype += 1;
      break;
    default:
      break;
  }
  return brtype;
}

void TAGE64K::HistoryUpdate(UINT64 PC, OpType opType, bool taken, UINT64 target) {
  int brtype = GetBrtypeFromOptype(opType);

  if (TAGESCL64KB_SC) {
#ifdef IMLI
    if (brtype & 1) {
#ifdef IMLI
      IMHIST[IMLIcount] = (IMHIST[IMLIcount] << 1) + taken;
#endif
      if (target < PC) {
        // This branch corresponds to a loop
        if (!taken) {
          // exit of the "loop"
          IMLIcount = 0;
        }
        if (taken) {
          if (IMLIcount < ((1 << Im[0]) - 1))
            IMLIcount++;
        }
      }
    }
#endif
    if (brtype & 1) {
      L_shist[INDLOCAL] = (L_shist[INDLOCAL] << 1) + (taken);
      S_slhist[INDSLOCAL] = ((S_slhist[INDSLOCAL] << 1) + taken) ^ (PC & 15);
      T_slhist[INDTLOCAL] = (T_slhist[INDTLOCAL] << 1) + taken;
    }
    // END UPDATE  HISTORIES
  }
}

void TAGE64K::SpecUpdateAtCond(UINT64 PC, bool dir, bool off_path) {
  if (TAGESCL64KB_LOOP)
    SpecLoopUpdate(PC, dir, Pstate.on_path_phist, Pstate.on_path_ptghist, off_path);

  if (TAGESCL64KB_SC) {
    bool SCPRED = (Pstate.LSUM >= 0);
    if ((SCPRED != dir) || ((abs(Pstate.LSUM) < Pstate.THRES))) {
      Gupdate((PC << 1) + Pstate.pred_inter, dir, Sstate.GHIST, Gm, Sstate.GGEHL, GNB, LOGGNB, Sstate.WG, Pstate.LSUM);
      Gupdate(PC, dir, Sstate.phist, Pm, Sstate.PGEHL, PNB, LOGPNB, Sstate.WP, Pstate.LSUM);
    }
  }
}

void TAGE64K::SpecUpdate(UINT64 PC, OpType opType, bool dir, UINT64 target) {
  int brtype = GetBrtypeFromOptype(opType);
  GlobalStateUpdate(PC, target, brtype, dir);
}

void TAGE64K::GlobalStateUpdate(UINT64 PC, UINT64 br_target, int brtype, bool pred_dir) {
  if (TAGESCL64KB_SC) {
    if (brtype & 1)
      Sstate.GHIST = (Sstate.GHIST << 1) + (pred_dir & (br_target < PC));
  }

  int T = ((PC ^ (PC >> 2))) ^ pred_dir;
  int PATH = PC ^ (PC >> 2) ^ (PC >> 4);
  // if branch is indirect, encode global history with 3 bits. Otherwise, 2 bits
  // int maxt = ((brtype & 2) != 0) ? 3 : 2;
  int maxt = 2;
  if (brtype & 1)
    maxt = 2;
  else if ((brtype & 2))
    maxt = 3;

  if ((brtype == 3) & pred_dir) {
    T = (T ^ (br_target >> 2));
    PATH = PATH ^ (br_target >> 2) ^ (br_target >> 4);
  }

  for (int t = 0; t < maxt; t++) {
    bool DIR = (T & 1);
    T >>= 1;
    int PATHBIT = (PATH & 127);
    PATH >>= 1;
    // update  history
    Sstate.ptghist--;  // pointer of global history
    Sstate.ghist[Sstate.ptghist & (HISTBUFFERLENGTH - 1)] = DIR;
    Sstate.phist = (Sstate.phist << 1) ^ PATHBIT;

    for (int i = 1; i <= NHIST; i++) {
      Sstate.ch_i[i].update(Sstate.ghist, Sstate.ptghist);
      Sstate.ch_t[0][i].update(Sstate.ghist, Sstate.ptghist);
      Sstate.ch_t[1][i].update(Sstate.ghist, Sstate.ptghist);
    }
  }
  Sstate.phist = (Sstate.phist & ((1 << PHISTWIDTH) - 1));  // path history
}

// PREDICTOR UPDATE
void TAGE64K::NonSpecUpdateAtCond(UINT64 PC, OpType opType, bool resolveDir, bool predDir, UINT64 branchTarget,
                                  Counter branch_id) {
  if (SPEC_LEVEL < BP_PRED_ONOFF_SPEC_UPDATE_S_ONOFF_UPDATE_N_ON)
    UpdatePredictor(PC, opType, resolveDir, predDir, branchTarget, Pstate);
  else {
    auto& key_index = predictor_states.get<0>();
    auto pit = key_index.find(branch_id);
    assert(pit != key_index.end());
    auto& mutable_state = const_cast<PredictorStates&>(pit->state);
    UpdatePredictor(PC, opType, resolveDir, predDir, branchTarget, mutable_state);
  }
}

// UpdatePredictor uses predictor state (pstate) from one of two sources:
// 1. For spec_level 0-3: Uses the global Pstate variable directly
// 2. For spec_level 4: Uses a snapshot from the predictor_states container
//    where each entry maintains a separate copy of predictor states
// This allows handling both on/off-path speculation while
// maintaining correct predictor state for each path.
void TAGE64K::UpdatePredictor(UINT64 PC, OpType opType, bool resolveDir, bool predDir, UINT64 branchTarget,
                              const PredictorStates& pstate) {
  if (TAGESCL64KB_LOOP) {
    if (pstate.LVALID) {
      if (pstate.pred_taken != pstate.predloop)
        ctrupdate(WITHLOOP, (pstate.predloop == resolveDir), 7);
    }
    LoopUpdate(PC, resolveDir, (pstate.pred_taken != resolveDir), pstate.LHIT, pstate.on_path_phist,
               pstate.on_path_ptghist);
  }

  if (TAGESCL64KB_SC) {
    bool SCPRED = (pstate.LSUM >= 0);
    if (pstate.pred_inter != SCPRED) {
      if ((abs(pstate.LSUM) < pstate.THRES))
        if ((pstate.HighConf)) {
          if ((abs(pstate.LSUM) < pstate.THRES / 2))
            if ((abs(pstate.LSUM) >= pstate.THRES / 4))
              ctrupdate(SecondH, (pstate.pred_inter == resolveDir), CONFWIDTH);
        }
      if ((pstate.MedConf))
        if ((abs(pstate.LSUM) < pstate.THRES / 4)) {
          ctrupdate(FirstH, (pstate.pred_inter == resolveDir), CONFWIDTH);
        }
    }

    if ((SCPRED != resolveDir) || ((abs(pstate.LSUM) < pstate.THRES))) {
      {
        if (SCPRED != resolveDir) {
          Pupdatethreshold[INDUPD] += 1;
          updatethreshold += 1;
        }

        else {
          Pupdatethreshold[INDUPD] -= 1;
          updatethreshold -= 1;
        }

        if (Pupdatethreshold[INDUPD] >= (1 << (WIDTHRESP - 1)))
          Pupdatethreshold[INDUPD] = (1 << (WIDTHRESP - 1)) - 1;
        // Pupdatethreshold[INDUPD] could be negative
        if (Pupdatethreshold[INDUPD] < -(1 << (WIDTHRESP - 1)))
          Pupdatethreshold[INDUPD] = -(1 << (WIDTHRESP - 1));
        if (updatethreshold >= (1 << (WIDTHRES - 1)))
          updatethreshold = (1 << (WIDTHRES - 1)) - 1;
        // updatethreshold could be negative
        if (updatethreshold < -(1 << (WIDTHRES - 1)))
          updatethreshold = -(1 << (WIDTHRES - 1));
      }
#ifdef VARTHRES
      {
        int XSUM =
            pstate.LSUM - ((WB[INDUPDS] >= 0) * ((2 * Bias[INDBIAS(pstate)] + 1) + (2 * BiasSK[INDBIASSK(pstate)] + 1) +
                                                 (2 * BiasBank[INDBIASBANK(pstate)] + 1)));
        if ((XSUM + ((2 * Bias[INDBIAS(pstate)] + 1) + (2 * BiasSK[INDBIASSK(pstate)] + 1) +
                     (2 * BiasBank[INDBIASBANK(pstate)] + 1)) >=
             0) != (XSUM >= 0))
          ctrupdate(WB[INDUPDS],
                    (((2 * Bias[INDBIAS(pstate)] + 1) + (2 * BiasSK[INDBIASSK(pstate)] + 1) +
                          (2 * BiasBank[INDBIASBANK(pstate)] + 1) >=
                      0) == resolveDir),
                    EWIDTH);
      }
#endif
    ctrupdate(Bias[INDBIAS(pstate)], resolveDir, PERCWIDTH);
    ctrupdate(BiasSK[INDBIASSK(pstate)], resolveDir, PERCWIDTH);
    ctrupdate(BiasBank[INDBIASBANK(pstate)], resolveDir, PERCWIDTH);
#ifdef LOCALH
    Gupdate(PC, resolveDir, L_shist[INDLOCAL], Lm, LGEHL, LNB, LOGLNB, WL, pstate.LSUM);
#ifdef LOCALS
    Gupdate(PC, resolveDir, S_slhist[INDSLOCAL], Sm, SGEHL, SNB, LOGSNB, WS, pstate.LSUM);
#endif
#ifdef LOCALT
    Gupdate(PC, resolveDir, T_slhist[INDTLOCAL], Tm, TGEHL, TNB, LOGTNB, WT, pstate.LSUM);
#endif
#endif

#ifdef IMLI
    Gupdate(PC, resolveDir, IMHIST[(IMLIcount)], IMm, IMGEHL, IMNB, LOGIMNB, WIM, pstate.LSUM);
    Gupdate(PC, resolveDir, IMLIcount, Im, IGEHL, INB, LOGINB, WI, pstate.LSUM);
#endif
    }
  }

  // TAGE UPDATE
  bool ALLOC = ((pstate.tage_pred != resolveDir) & (pstate.HitBank < NHIST));
  int hit_idx = pstate.HitBank;
  int alt_idx = pstate.AltBank;
  if (SPEC_LEVEL >= BP_PRED_ONOFF_SPEC_UPDATE_S_ONOFF_UPDATE_N_ON) {
    hit_idx = noskip_index[pstate.HitBank];
    if (TAGESCL64KB_ALT)
      alt_idx = noskip_index[pstate.AltBank];
  }

  // do not allocate too often if the overall prediction is correct

  if (pstate.HitBank > 0) {
    // Manage the selection between longest matching and alternate matching
    // for "pseudo"-newly allocated longest matching entry
    // this is extremely important for TAGE only, not that important when the overall predictor is implemented
    bool PseudoNewAlloc = (abs(2 * gtable[pstate.HitBank][pstate.GI[hit_idx]].ctr + 1) <= 1);
    // an entry is considered as newly allocated if its prediction counter is weak
    if (PseudoNewAlloc) {
      if (pstate.LongestMatchPred == resolveDir)
        ALLOC = false;
      // if it was delivering the correct prediction, no need to allocate a new entry
      // even if the overall prediction was false
      if (TAGESCL64KB_ALT) {
        if (pstate.LongestMatchPred != pstate.alttaken) {
          ctrupdate(use_alt_on_na[INDUSEALT(pstate)], (pstate.alttaken == resolveDir), ALTWIDTH);
        }
      }
    }
  }

  if (pstate.pred_taken == resolveDir)
    if ((MYRANDOM(pstate.on_path_phist, pstate.on_path_ptghist, false) & 31) != 0)
      ALLOC = false;

  if (ALLOC) {
    int T = NNN;

    int A = 1;
    if ((MYRANDOM(pstate.on_path_phist, pstate.on_path_ptghist, false) & 127) < 32)
      A = 2;
    int Penalty = 0;
    int NA = 0;
    int DEP = ((((pstate.HitBank - 1 + 2 * A) & 0xffe)) ^
               (MYRANDOM(pstate.on_path_phist, pstate.on_path_ptghist, false) & 1));
    // just a complex formula to chose between X and X+1, when X is odd: sorry

    for (int I = DEP; I < NHIST; I += 2) {
      int i = I + 1;
      bool Done = false;
      if (NOSKIP[i]) {
        int idx = i;
        if (SPEC_LEVEL >= BP_PRED_ONOFF_SPEC_UPDATE_S_ONOFF_UPDATE_N_ON)
          idx = noskip_index[i];
        if (gtable[i][pstate.GI[idx]].u == 0) {
#define OPTREMP
          // the replacement is optimized with a single u bit: 0.2 %
#ifdef OPTREMP
          if (abs(2 * gtable[i][pstate.GI[idx]].ctr + 1) <= 3)
#endif
          {
            gtable[i][pstate.GI[idx]].tag = pstate.GTAG[idx];
            gtable[i][pstate.GI[idx]].ctr = (resolveDir) ? 0 : -1;
            NA++;
            if (T <= 0) {
              break;
            }
            I += 2;
            Done = true;
            T -= 1;
          }
#ifdef OPTREMP
          else {
            if (gtable[i][pstate.GI[idx]].ctr > 0)
              gtable[i][pstate.GI[idx]].ctr--;
            else
              gtable[i][pstate.GI[idx]].ctr++;
          }

#endif

        }

        else {
          Penalty++;
        }
      }

      if (!Done) {
        i = (I ^ 1) + 1;
        if (NOSKIP[i]) {
          int idx = i;
          if (SPEC_LEVEL >= BP_PRED_ONOFF_SPEC_UPDATE_S_ONOFF_UPDATE_N_ON)
            idx = noskip_index[i];

          if (gtable[i][pstate.GI[idx]].u == 0) {
#ifdef OPTREMP
            if (abs(2 * gtable[i][pstate.GI[idx]].ctr + 1) <= 3)
#endif

            {
              gtable[i][pstate.GI[idx]].tag = pstate.GTAG[idx];
              gtable[i][pstate.GI[idx]].ctr = (resolveDir) ? 0 : -1;
              NA++;
              if (T <= 0) {
                break;
              }
              I += 2;
              T -= 1;
            }
#ifdef OPTREMP
            else {
              if (gtable[i][pstate.GI[idx]].ctr > 0)
                gtable[i][pstate.GI[idx]].ctr--;
              else
                gtable[i][pstate.GI[idx]].ctr++;
            }

#endif

          } else {
            Penalty++;
          }
        }
      }
    }
    TICK += (Penalty - 2 * NA);

    // just the best formula for the Championship:
    // In practice when one out of two entries are useful
    if (TICK < 0)
      TICK = 0;
    if (TICK >= BORNTICK) {
      for (int i = 1; i <= BORN; i += BORN - 1)
        for (int j = 0; j < SizeTable[i]; j++)
          gtable[i][j].u >>= 1;
      TICK = 0;
    }
  }

  // update predictions
  if (pstate.HitBank > 0) {
    if (abs(2 * gtable[pstate.HitBank][pstate.GI[hit_idx]].ctr + 1) == 1)
      if (pstate.LongestMatchPred != resolveDir) {  // acts as a protection
        if (TAGESCL64KB_ALT) {
          if (alt_idx > 0) {
            ctrupdate(gtable[pstate.AltBank][pstate.GI[alt_idx]].ctr, resolveDir, CWIDTH);
          }
          if (alt_idx == 0)
            baseupdate(resolveDir, PC);
        } else
          baseupdate(resolveDir, PC);
      }
    ctrupdate(gtable[pstate.HitBank][pstate.GI[hit_idx]].ctr, resolveDir, CWIDTH);
    // sign changes: no way it can have been useful
    if (abs(2 * gtable[pstate.HitBank][pstate.GI[hit_idx]].ctr + 1) == 1)
      gtable[pstate.HitBank][pstate.GI[hit_idx]].u = 0;
    if (TAGESCL64KB_ALT) {
      if (pstate.alttaken == resolveDir)
        if (alt_idx > 0)
          if (abs(2 * gtable[pstate.AltBank][pstate.GI[alt_idx]].ctr + 1) == 7)
            if (gtable[pstate.HitBank][pstate.GI[hit_idx]].u == 1) {
              if (pstate.LongestMatchPred == resolveDir) {
                gtable[pstate.HitBank][pstate.GI[hit_idx]].u = 0;
              }
            }
    }
  }
  else
    baseupdate(resolveDir, PC);

  if (pstate.HitBank > 0) {
    if (TAGESCL64KB_ALT) {
      if (pstate.LongestMatchPred != pstate.alttaken)
        if (pstate.LongestMatchPred == resolveDir) {
          if (gtable[pstate.HitBank][pstate.GI[hit_idx]].u < (1 << UWIDTH) - 1)
            gtable[pstate.HitBank][pstate.GI[hit_idx]].u++;
        }
    } else if (pstate.LongestMatchPred == resolveDir) {
      if (gtable[pstate.HitBank][pstate.GI[hit_idx]].u < (1 << UWIDTH) - 1)
        gtable[pstate.HitBank][pstate.GI[hit_idx]].u++;
    }
  }

  // END TAGE UPDATE
  HistoryUpdate(PC, opType, resolveDir, branchTarget);
}

#define GINDEX                                                                                        \
  (((long long)PC) ^ bhist ^ (bhist >> (8 - i)) ^ (bhist >> (16 - 2 * i)) ^ (bhist >> (24 - 3 * i)) ^ \
   (bhist >> (32 - 3 * i)) ^ (bhist >> (40 - 4 * i))) &                                               \
      ((1 << (logs - (i >= (NBR - 2)))) - 1)
int TAGE64K::Gpredict(UINT64 PC, long long BHIST, int* length, int8_t** tab, int NBR, int logs, int8_t* W) {
  int PERCSUM = 0;
  for (int i = 0; i < NBR; i++) {
    long long bhist = BHIST & ((long long)((1 << length[i]) - 1));
    long long index = GINDEX;

    int8_t ctr = tab[i][index];

    PERCSUM += (2 * ctr + 1);
  }
#ifdef VARTHRES
  PERCSUM = (1 + (W[INDUPDS] >= 0)) * PERCSUM;
#endif
  return ((PERCSUM));
}
void TAGE64K::Gupdate(UINT64 PC, bool taken, long long BHIST, int* length, int8_t** tab, int NBR, int logs, int8_t* W,
                      int LSUM) {
  int PERCSUM = 0;

  for (int i = 0; i < NBR; i++) {
    long long bhist = BHIST & ((long long)((1 << length[i]) - 1));
    long long index = GINDEX;

    PERCSUM += (2 * tab[i][index] + 1);
    ctrupdate(tab[i][index], taken, PERCWIDTH);
  }
#ifdef VARTHRES
  {
    int XSUM = LSUM - ((W[INDUPDS] >= 0)) * PERCSUM;
    if ((XSUM + PERCSUM >= 0) != (XSUM >= 0))
      ctrupdate(W[INDUPDS], ((PERCSUM >= 0) == taken), EWIDTH);
  }
#endif
}

void TAGE64K::TrackOtherInst(UINT64 PC, OpType opType, bool taken, UINT64 branchTarget) {
  HistoryUpdate(PC, opType, taken, branchTarget);
}

int TAGE64K::lindex(UINT64 PC) {
  return (((PC ^ (PC >> 2)) & ((1 << (LOGL - 2)) - 1)) << 2);
}

// loop prediction: only used if high confidence
// skewed associative 4-way
// At fetch time: speculative
#define CONFLOOP 15
bool TAGE64K::getloop(UINT64 PC) {
  Pstate.LHIT = -1;

  LI = lindex(PC);
  LIB = ((PC >> (LOGL - 2)) & ((1 << (LOGL - 2)) - 1));
  LTAG = (PC >> (LOGL - 2)) & ((1 << 2 * LOOPTAG) - 1);
  LTAG ^= (LTAG >> LOOPTAG);
  LTAG = (LTAG & ((1 << LOOPTAG) - 1));

  for (int i = 0; i < 4; i++) {
    int index = (LI ^ ((LIB >> i) << 2)) + i;

    if (Sstate.ltable[index].TAG == LTAG) {
      Pstate.LHIT = i;
      Pstate.LVALID = ((Sstate.ltable[index].confid == CONFLOOP) ||
                       (Sstate.ltable[index].confid * Sstate.ltable[index].NbIter > 128));

      if (Sstate.ltable[index].CurrentIter + 1 == Sstate.ltable[index].NbIter)
        return (!(Sstate.ltable[index].dir));
      return ((Sstate.ltable[index].dir));
    }
  }

  Pstate.LVALID = false;
  return (false);
}

void TAGE64K::SpecLoopUpdate(UINT64 PC, bool Taken, long long on_path_phist, int on_path_ptghist, bool off_path) {
  if (Pstate.LHIT < 0)
    return;
  // Calculate index into loop predictor table
  int index = (LI ^ ((LIB >> Pstate.LHIT) << 2)) + Pstate.LHIT;
  auto& entry = Sstate.ltable[index];

  // Handle valid loop prediction
  if (Pstate.LVALID) {
    if (Taken != Pstate.predloop) {
      // Free the entry on misprediction
      entry.NbIter = 0;
      entry.age = 0;
      entry.confid = 0;
      entry.CurrentIter = 0;
      return;
    }
    // Increment age when prediction differs from TAGE or randomly
    if ((Pstate.predloop != Pstate.tage_pred) || ((MYRANDOM(on_path_phist, on_path_ptghist, off_path) & 7) == 0))
      if (entry.age < CONFLOOP)
        entry.age++;
  }

  // Update iteration counter
  entry.CurrentIter++;
  entry.CurrentIter &= ((1 << WIDTHNBITERLOOP) - 1);

  // Reset on overflow
  if (entry.CurrentIter > entry.NbIter) {
    entry.confid = 0;
    entry.NbIter = 0;
  }

  // Handle direction change
  if (Taken != entry.dir) {
    if (entry.CurrentIter == entry.NbIter) {
      // Increment confidence if not saturated
      if (entry.confid < CONFLOOP)
        entry.confid++;
      // Free entry for small loops (1-2 iterations)
      if (entry.NbIter < 3) {
        entry.dir = Taken;
        entry.NbIter = 0;
        entry.age = 0;
        entry.confid = 0;
      }
    } else {
      // Handle first complete iteration or mismatch
      if (entry.NbIter == 0) {
        entry.confid = 0;
        entry.NbIter = entry.CurrentIter;
      } else {
        // Free entry on iteration count mismatch
        entry.NbIter = 0;
        entry.confid = 0;
      }
    }
    entry.CurrentIter = 0;
  }
}

void TAGE64K::LoopUpdate(UINT64 PC, bool Taken, bool ALLOC, int lhit, long long on_path_phist, int on_path_ptghist) {
  if (lhit >= 0) {
    return;
  }

  if (!ALLOC)
    return;

  UINT64 X = MYRANDOM(on_path_phist, on_path_ptghist, false) & 3;
  // 25% chance to attempt allocation
  if ((MYRANDOM(on_path_phist, on_path_ptghist, false) & 3) != 0)
    return;
  for (int i = 0; i < 4; i++) {
    int LHIT = (X + i) & 3;
    int index = (LI ^ ((LIB >> LHIT) << 2)) + LHIT;
    if (Sstate.ltable[index].age == 0) {
      Sstate.ltable[index].dir = !Taken;
      // most of mispredictions are on last iterations
      Sstate.ltable[index].TAG = LTAG;
      Sstate.ltable[index].NbIter = 0;
      Sstate.ltable[index].age = 7;
      Sstate.ltable[index].confid = 0;
      Sstate.ltable[index].CurrentIter = 0;
      break;
    } else
      Sstate.ltable[index].age--;
    break;
  }
}

void TAGE64K::ComparePredictor(const PredictorStates& state) {
  if (TAGESCL64KB_SC) {
    ASSERTM(0, Pstate.LSUM == state.LSUM, "id: %llu LSUM mismatch: %d vs %d", branch_id, Pstate.LSUM, state.LSUM);
    ASSERTM(0, Pstate.THRES == state.THRES, "id: %llu THRES mismatch: %d vs %d", branch_id, Pstate.THRES, state.THRES);
  }
  if (TAGESCL64KB_LOOP) {
    ASSERTM(0, Pstate.predloop == state.predloop, "id: %llu predloop mismatch", branch_id);
    ASSERTM(0, Pstate.LHIT == state.LHIT, "id: %llu LHIT mismatch: %d vs %d", branch_id, Pstate.LHIT, state.LHIT);
    ASSERTM(0, Pstate.LVALID == state.LVALID, "id: %llu LVALID mismatch", branch_id);
  }
  ASSERTM(0, Pstate.pred_taken == state.pred_taken, "id: %llu pred_taken mismatch", branch_id);
  ASSERTM(0, Pstate.tage_pred == state.tage_pred, "tage_pred mismatch");
  ASSERTM(0, Pstate.pred_inter == state.pred_inter, "pred_inter mismatch");
  ASSERTM(0, Pstate.LongestMatchPred == state.LongestMatchPred, "LongestMatchPred mismatch");
  ASSERTM(0, Pstate.HighConf == state.HighConf, "HighConf mismatch");
  ASSERTM(0, Pstate.MedConf == state.MedConf, "MedConf mismatch");
  ASSERTM(0, Pstate.LowConf == state.LowConf, "LowConf mismatch");
  ASSERTM(0, Pstate.HitBank == state.HitBank, "id: %llu HitBank mismatch: %d vs %d", branch_id, Pstate.HitBank,
          state.HitBank);
  if (TAGESCL64KB_ALT) {
    ASSERTM(0, Pstate.alttaken == state.alttaken, "alttaken mismatch");
    ASSERTM(0, Pstate.AltConf == state.AltConf, "AltConf mismatch");
    ASSERTM(0, Pstate.AltBank == state.AltBank, "id: %llu AltBank mismatch: %d vs %d", branch_id, Pstate.AltBank,
            state.AltBank);
  }
  // Example how to print all states
  // ASSERTM(0, Pstate.HitBank == state.HitBank,
  //         "id: %llu HitBank mismatch: %d vs %d\n"
  //         "Pstate: %s\n"
  //         "_Pstate: %s\n"
  //         "Sstate: %s\n"
  //         "_Sstate: %s",
  //         branch_id, Pstate.HitBank, state.HitBank,
  //         Pstate.to_string().c_str(),
  //         state.to_string().c_str(),
  //         Sstate.to_string().c_str(),
  //         state.to_string().c_str());

  // Compare GI and GTAG arrays
  int j = 0;
  for (int i = 0; i <= NHIST; i++) {
    if (NOSKIP[i]) {
      ASSERTM(0, Pstate.GI[i] == state.GI[j], "GI[%d] mismatch: %d vs %d", i, Pstate.GI[i], state.GI[j]);
      ASSERTM(0, Pstate.GTAG[i] == state.GTAG[j], "GTAG[%d] mismatch: %u vs %u", i, Pstate.GTAG[i], state.GTAG[j]);
      j++;
    }
  }
}

void TAGE64K::CompareCheckpoint(const Checkpoint& state) {
  ASSERTM(0, Sstate.ptghist == state.ptghist, "ptghist mismatch: %d vs %d", Sstate.ptghist, state.ptghist);
  ASSERTM(0, Sstate.phist == state.phist, "phist mismatch: %lld vs %lld", Sstate.phist, state.phist);

  // Compare folded histories
  for (int i = 0; i <= NHIST; i++) {
    ASSERTM(0, Sstate.ch_i[i].comp == state.ch_i[i].comp, "ch_i[%d].comp mismatch: %u vs %u", i, Sstate.ch_i[i].comp,
            state.ch_i[i].comp);
    ASSERTM(0, Sstate.ch_i[i].CLENGTH == state.ch_i[i].CLENGTH, "ch_i[%d].CLENGTH mismatch: %d vs %d", i,
            Sstate.ch_i[i].CLENGTH, state.ch_i[i].CLENGTH);
    ASSERTM(0, Sstate.ch_i[i].OLENGTH == state.ch_i[i].OLENGTH, "ch_i[%d].OLENGTH mismatch: %d vs %d", i,
            Sstate.ch_i[i].OLENGTH, state.ch_i[i].OLENGTH);
    ASSERTM(0, Sstate.ch_i[i].OUTPOINT == state.ch_i[i].OUTPOINT, "ch_i[%d].OUTPOINT mismatch: %d vs %d", i,
            Sstate.ch_i[i].OUTPOINT, state.ch_i[i].OUTPOINT);

    for (int j = 0; j < 2; j++) {
      ASSERTM(0, Sstate.ch_t[j][i].comp == state.ch_t[j][i].comp, "ch_t[%d][%d].comp mismatch: %u vs %u", j, i,
              Sstate.ch_t[j][i].comp, state.ch_t[j][i].comp);
      ASSERTM(0, Sstate.ch_t[j][i].CLENGTH == state.ch_t[j][i].CLENGTH, "ch_t[%d][%d].CLENGTH mismatch: %d vs %d", j, i,
              Sstate.ch_t[j][i].CLENGTH, state.ch_t[j][i].CLENGTH);
      ASSERTM(0, Sstate.ch_t[j][i].OLENGTH == state.ch_t[j][i].OLENGTH, "ch_t[%d][%d].OLENGTH mismatch: %d vs %d", j, i,
              Sstate.ch_t[j][i].OLENGTH, state.ch_t[j][i].OLENGTH);
      ASSERTM(0, Sstate.ch_t[j][i].OUTPOINT == state.ch_t[j][i].OUTPOINT, "ch_t[%d][%d].OUTPOINT mismatch: %d vs %d", j,
              i, Sstate.ch_t[j][i].OUTPOINT, state.ch_t[j][i].OUTPOINT);
    }
  }

  if (TAGESCL64KB_SC) {
    ASSERTM(0, Sstate.GHIST == state.GHIST, "GHIST mismatch: %lld vs %lld", Sstate.GHIST, state.GHIST);
    for (int i = 0; i < GNB; i++) {
      for (int j = 0; j < (1 << LOGGNB); j++) {
        ASSERTM(0, Sstate.GGEHL[i][j] == state.GGEHL[i][j], "GGEHL[%d][%d] mismatch: %d vs %d", i, j,
                Sstate.GGEHL[i][j], state.GGEHL[i][j]);
      }
    }

    for (int i = 0; i < PNB; i++) {
      for (int j = 0; j < (1 << LOGPNB); j++) {
        ASSERTM(0, Sstate.PGEHL[i][j] == state.PGEHL[i][j], "PGEHL[%d][%d] mismatch: %d vs %d", i, j,
                Sstate.PGEHL[i][j], state.PGEHL[i][j]);
      }
    }

    // Compare weight tables
    for (int i = 0; i < (1 << LOGSIZEUPS); i++) {
      ASSERTM(0, Sstate.WG[i] == state.WG[i], "WG[%d] mismatch: %d vs %d", i, Sstate.WG[i], state.WG[i]);
      ASSERTM(0, Sstate.WP[i] == state.WP[i], "WP[%d] mismatch: %d vs %d", i, Sstate.WP[i], state.WP[i]);
    }
  }
}