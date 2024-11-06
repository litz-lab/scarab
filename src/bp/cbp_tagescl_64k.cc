#include "cbp_tagescl_64k.h"

TAGE64K::TAGE64K(void) {
  reinit();
}

TAGE64K& TAGE64K::operator=(const TAGE64K& other) {
  memcpy(Bias, other.Bias, sizeof(Bias));
  memcpy(BiasSK, other.BiasSK, sizeof(BiasSK));
  memcpy(BiasBank, other.BiasBank, sizeof(BiasBank));
  memcpy(Im, other.Im, sizeof(Im));
  memcpy(IGEHLA, other.IGEHLA, sizeof(IGEHLA));
  memcpy(IGEHL, other.IGEHL, sizeof(IGEHL));
  memcpy(IMm, other.IMm, sizeof(IMm));
  memcpy(IMGEHLA, other.IMGEHLA, sizeof(IMGEHLA));
  memcpy(IMGEHL, other.IMGEHL, sizeof(IMGEHL));
  memcpy(IMHIST, other.IMHIST, sizeof(IMHIST));
  memcpy(Gm, other.Gm, sizeof(Gm));
  memcpy(GGEHLA, other.GGEHLA, sizeof(GGEHLA));
  memcpy(GGEHL, other.GGEHL, sizeof(GGEHL));
  memcpy(Pm, other.Pm, sizeof(Pm));
  memcpy(PGEHLA, other.PGEHLA, sizeof(PGEHLA));
  memcpy(PGEHL, other.PGEHL, sizeof(PGEHL));
  memcpy(Lm, other.Lm, sizeof(Lm));
  memcpy(LGEHLA, other.LGEHLA, sizeof(LGEHLA));
  memcpy(LGEHL, other.LGEHL, sizeof(LGEHL));
  memcpy(L_shist, other.L_shist, sizeof(L_shist));
  memcpy(Sm, other.Sm, sizeof(Sm));
  memcpy(SGEHLA, other.SGEHLA, sizeof(SGEHLA));
  memcpy(SGEHL, other.SGEHL, sizeof(SGEHL));
  memcpy(S_slhist, other.S_slhist, sizeof(S_slhist));
  memcpy(Tm, other.Tm, sizeof(Tm));
  memcpy(TGEHLA, other.TGEHLA, sizeof(TGEHLA));
  memcpy(TGEHL, other.TGEHL, sizeof(TGEHL));
  memcpy(T_slhist, other.T_slhist, sizeof(T_slhist));
  updatethreshold = other.updatethreshold;
  memcpy(Pupdatethreshold, other.Pupdatethreshold, sizeof(Pupdatethreshold));
  memcpy(WG, other.WG, sizeof(WG));
  memcpy(WL, other.WL, sizeof(WL));
  memcpy(WS, other.WS, sizeof(WS));
  memcpy(WT, other.WT, sizeof(WT));
  memcpy(WP, other.WP, sizeof(WP));
  memcpy(WI, other.WI, sizeof(WI));
  memcpy(WIM, other.WIM, sizeof(WIM));
  memcpy(WB, other.WB, sizeof(WB));
  FirstH = other.FirstH;
  SecondH = other.SecondH;
  memcpy(SizeTable, other.SizeTable, sizeof(SizeTable));
  memcpy(NOSKIP, other.NOSKIP, sizeof(NOSKIP));
  Pstate.THRES = other.Pstate.THRES;
  Pstate.LSUM = other.Pstate.LSUM;
  Pstate.predloop = other.Pstate.predloop;
  Pstate.LHIT = other.Pstate.LHIT;
  Pstate.LVALID= other.Pstate.LVALID;
  Pstate.pred_taken = other.Pstate.pred_taken;
  Pstate.tage_pred = other.Pstate.tage_pred;
  Pstate.pred_inter = other.Pstate.pred_inter;
  Pstate.LongestMatchPred = other.Pstate.LongestMatchPred;
  Pstate.alttaken = other.Pstate.alttaken;
  Pstate.HighConf = other.Pstate.HighConf;
  Pstate.MedConf = other.Pstate.MedConf;
  Pstate.LowConf = other.Pstate.LowConf;
  Pstate.AltConf = other.Pstate.AltConf;
  Pstate.HitBank = other.Pstate.HitBank;
  Pstate.AltBank = other.Pstate.AltBank;
  memcpy(Pstate.GI.get(), other.Pstate.GI.get(), (NHIST+1)*sizeof(int));
  memcpy(Pstate.GTAG.get(), other.Pstate.GTAG.get(), (NHIST+1)*sizeof(uint));
  LIB = other.LIB;
  LI = other.LI;
  LTAG = other.LTAG;
  WITHLOOP = other.WITHLOOP;
  memcpy(use_alt_on_na, other.use_alt_on_na, sizeof(use_alt_on_na));
  TICK = other.TICK;
  for (int i = 0; i < (1<<(LOGL)); ++i) {
    ltable[i].NbIter = other.ltable[i].NbIter;
    ltable[i].confid = other.ltable[i].confid;
    ltable[i].CurrentIter = other.ltable[i].CurrentIter;
    ltable[i].TAG = other.ltable[i].TAG;
    ltable[i].age = other.ltable[i].age;
    ltable[i].dir = other.ltable[i].dir;
  }
  GHIST = other.GHIST;
  memcpy(ghist, other.ghist, sizeof(ghist));
  ptghist = other.ptghist;
  phist = other.phist;
  for (int i = 0; i < NHIST+1; ++i) {
    ch_i[i].comp = other.ch_i[i].comp;
    ch_i[i].CLENGTH = other.ch_i[i].CLENGTH;
    ch_i[i].OLENGTH = other.ch_i[i].OLENGTH;
    ch_i[i].OUTPOINT = other.ch_i[i].OUTPOINT;
  }
  for (int i = 0; i < 2; ++i) {
    for (int j = 0; j < NHIST+1; ++j) {
      ch_t[i][j].comp = other.ch_t[i][j].comp;
      ch_t[i][j].CLENGTH = other.ch_t[i][j].CLENGTH;
      ch_t[i][j].OLENGTH = other.ch_t[i][j].OLENGTH;
      ch_t[i][j].OUTPOINT = other.ch_t[i][j].OUTPOINT;
    }
  }
  for (int i = 0; i < (1<<(LOGB)); ++i) {
    btable[i].hyst = other.btable[i].hyst;
    btable[i].pred = other.btable[i].pred;
  }
  for (int i = 1; i < BORN; ++i) {
    for (int j = 0; j < NBANKLOW * (1 << LOGG); ++j) {
      gtable[i][j].ctr = other.gtable[i][j].ctr;
      gtable[i][j].tag = other.gtable[i][j].tag;
      gtable[i][j].u = other.gtable[i][j].u;
    }
  }
  for (int i = BORN; i < NHIST+1; ++i) {
    for (int j = 0; j < NBANKHIGH * (1 << LOGG); ++j) {
      gtable[i][j].ctr = other.gtable[i][j].ctr;
      gtable[i][j].tag = other.gtable[i][j].tag;
      gtable[i][j].u = other.gtable[i][j].u;
    }
  }

  memcpy(m, other.m, sizeof(m));
  memcpy(TB, other.TB, sizeof(TB));
  memcpy(logg, other.logg, sizeof(logg));
  Seed = other.Seed;
  IMLIcount = other.IMLIcount;
  tage_component = other.tage_component;
  tage_component_inter = other.tage_component_inter;
  tage_component_tage = other.tage_component_tage;
  tage_component_alt = other.tage_component_alt;
  // no need to copy checkpoints and predictor_states ?
  branch_id = other.branch_id;
  off_path = other.off_path;
  condpattern = other.condpattern;
  memcpy(noskip_index, other.noskip_index, sizeof(noskip_index));

  return *this;
}

uns8 TAGE64K::IsFull(void) {
  return 0;
}

void TAGE64K::reinit() {
  m[1]         = MINHIST;
  m[NHIST / 2] = MAXHIST;
  for (int i = 2; i <= NHIST / 2; i++)
    m[i] = (int) (((double) MINHIST * pow((double) (MAXHIST) / (double) MINHIST, (double) (i - 1) / (double) (((NHIST / 2) - 1)))) + 0.5);

  for (int i = 1; i <= NHIST; i++)
    NOSKIP[i] = ((i - 1) & 1) || ((i >= BORNINFASSOC) & (i < BORNSUPASSOC));

  // just eliminate some extra tables (very very marginal)
  NOSKIP[4]         = 0;
  NOSKIP[NHIST - 2] = 0;
  NOSKIP[8]         = 0;
  NOSKIP[NHIST - 6] = 0;
  int idx_n         = 0;
  for (int i = 0; i <= NHIST; i++) {
    if (NOSKIP[i])
      noskip_index[i] = idx_n++;
    else
      noskip_index[i] = -1;
  }
  ASSERTM(0, idx_n == NOSKIPCNT, "incorrect idx_n: %d", idx_n);

  for (int i = NHIST; i > 1; i--)
    m[i] = m[(i + 1) / 2];

  for (int i = 1; i <= NHIST; i++) {
    TB[i]   = TBITS + 4 * (i >= BORN);
    logg[i] = LOGG;
  }

#ifdef LOOPPREDICTOR
  ltable = new cbp64_lentry[1 << (LOGL)];
#endif

  gtable[1]    = new cbp64_gentry[NBANKLOW * (1 << LOGG)];
  SizeTable[1] = NBANKLOW * (1 << LOGG);

  gtable[BORN]    = new cbp64_gentry[NBANKHIGH * (1 << LOGG)];
  SizeTable[BORN] = NBANKHIGH * (1 << LOGG);

  for (int i = BORN + 1; i <= NHIST; i++)
    gtable[i] = gtable[BORN];

  for (int i = 2; i <= BORN - 1; i++)
    gtable[i] = gtable[1];

  btable = new cbp64_bentry[1 << LOGB];

  for (int i = 1; i <= NHIST; i++) {
    ch_i[i].init(m[i], (logg[i]));
    ch_t[0][i].init(ch_i[i].OLENGTH, TB[i]);
    ch_t[1][i].init(ch_i[i].OLENGTH, TB[i] - 1);
  }
#ifdef LOOPPREDICTOR
  Pstate.LVALID = false;
  WITHLOOP      = -1;
#endif

  for (int i = 0; i < HISTBUFFERLENGTH; i++)
    ghist[0] = 0;

  ptghist         = 0;
  updatethreshold = 35 << 3;

  for (int i = 0; i < (1 << LOGSIZEUP); i++)
    Pupdatethreshold[i] = 0;
  for (int i = 0; i < GNB; i++)
    GGEHL[i] = &GGEHLA[i][0];
  for (int i = 0; i < LNB; i++)
    LGEHL[i] = &LGEHLA[i][0];

  for (int i = 0; i < GNB; i++)
    for (int j = 0; j < ((1 << LOGGNB) - 1); j++)
      if (!(j & 1))
        GGEHL[i][j] = -1;

  for (int i = 0; i < LNB; i++)
    for (int j = 0; j < ((1 << LOGLNB) - 1); j++)
      if (!(j & 1))
        LGEHL[i][j] = -1;

  for (int i = 0; i < SNB; i++)
    SGEHL[i] = &SGEHLA[i][0];
  for (int i = 0; i < TNB; i++)
    TGEHL[i] = &TGEHLA[i][0];
  for (int i = 0; i < PNB; i++)
    PGEHL[i] = &PGEHLA[i][0];
#ifdef IMLI
#ifdef IMLIOH
  for (int i = 0; i < FNB; i++)
    FGEHL[i] = &FGEHLA[i][0];

  for (int i = 0; i < FNB; i++)
    for (int j = 0; j < ((1 << LOGFNB) - 1); j++)
      if (!(j & 1))
        FGEHL[i][j] = -1;
#endif
  for (int i = 0; i < INB; i++)
    IGEHL[i] = &IGEHLA[i][0];

  for (int i = 0; i < INB; i++)
    for (int j = 0; j < ((1 << LOGINB) - 1); j++)
      if (!(j & 1))
        IGEHL[i][j] = -1;

  for (int i = 0; i < IMNB; i++)
    IMGEHL[i] = &IMGEHLA[i][0];

  for (int i = 0; i < IMNB; i++)
    for (int j = 0; j < ((1 << LOGIMNB) - 1); j++)
      if (!(j & 1))
        IMGEHL[i][j] = -1;

#endif
  for (int i = 0; i < SNB; i++)
    for (int j = 0; j < ((1 << LOGSNB) - 1); j++)
      if (!(j & 1))
        SGEHL[i][j] = -1;

  for (int i = 0; i < TNB; i++)
    for (int j = 0; j < ((1 << LOGTNB) - 1); j++)
      if (!(j & 1))
        TGEHL[i][j] = -1;

  for (int i = 0; i < PNB; i++)
    for (int j = 0; j < ((1 << LOGPNB) - 1); j++)
      if (!(j & 1))
        PGEHL[i][j] = -1;

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
  for (int i = 0; i < SIZEUSEALT; i++)
    use_alt_on_na[i] = 0;

  for (int i = 0; i < (1 << LOGSIZEUPS); i++) {
    WG[i] = 7;
    WL[i] = 7;
    WS[i] = 7;
    WT[i] = 7;
    WP[i] = 7;
    WI[i] = 7;
    WB[i] = 4;
  }
  for (int i = 0; i < NLOCAL; i++)
    L_shist[i] = 0;
  for (int i = 0; i < NSECLOCAL; i++)
    S_slhist[i] = 0;

  TICK    = 0;
  GHIST   = 0;
  ptghist = 0;
  phist   = 0;
  Seed    = 0x123456789ABCDEF0ULL;

  tage_component     = TAGE_BASE;
  tage_component_alt = TAGE_BASE;
  branch_id          = 0;
  condpattern        = 0xc2b2ae35;
  off_path           = false;
}

Counter TAGE64K::KeyGeneration() {
  return ++branch_id;
}

// index function for the bimodal table
int TAGE64K::bindex(UINT64 PC) {
  return ((PC ^ (PC >> LOGB)) & ((1 << (LOGB)) - 1));
}

// the index functions for the tagged tables uses path history as in the OGEHL predictor
// F serves to mix path history: not very important impact
int TAGE64K::F(long long A, int size, int bank) {
  int A1, A2;
  A  = A & ((1 << size) - 1);
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
int TAGE64K::gindex(unsigned int PC, int bank, long long hist,
    cbp64_folded_history* ch_i) {
  int index;
  int M = (m[bank] > PHISTWIDTH) ? PHISTWIDTH : m[bank];
  index = PC ^ (PC >> (abs(logg[bank] - bank) + 1)) ^ ch_i[bank].comp ^ F(hist, M, bank);

  return (index & ((1 << (logg[bank])) - 1));
}

//  tag computation
uint16_t TAGE64K::gtag(unsigned int PC, int bank, cbp64_folded_history* ch0,
    cbp64_folded_history* ch1) {
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
  int    BI  = GET_BI(PC);
  int8_t BIM = (btable[BI].pred << 1) + (btable[BI >> HYSTSHIFT].hyst);

  BIM             = (btable[BI].pred << 1) + (btable[BI >> HYSTSHIFT].hyst);
  Pstate.HighConf = (BIM == 0) || (BIM == 3);
  Pstate.LowConf  = !Pstate.HighConf;
  Pstate.AltConf  = Pstate.HighConf;
  Pstate.MedConf  = false;
  return (btable[BI].pred > 0);
}

void TAGE64K::baseupdate(bool Taken, UINT64 PC, cbp64_bentry* btable) {
  int    BI  = GET_BI(PC);
  int8_t BIM = (btable[BI].pred << 1) + (btable[BI >> HYSTSHIFT].hyst);

  int inter = BIM;
  if (Taken) {
    if (inter < 3)
      inter += 1;
  } else if (inter > 0)
    inter--;
  btable[BI].pred              = inter >> 1;
  btable[BI >> HYSTSHIFT].hyst = (inter & 1);
};

// just a simple pseudo random number generator: use available information
//  to allocate entries  in the loop predictor
Counter TAGE64K::MYRANDOM(Counter opnum) {
  Seed++;
  Seed ^= condpattern;
  Seed = (Seed >> 21) + (Seed << 11);
  Seed ^= opnum;
  Seed = (Seed >> 10) + (Seed << 22);
  return (Seed);
};

//  TAGE PREDICTION: same code at fetch or retire time but the index and tags must recomputed
void TAGE64K::Tagepred(UINT64 PC) {
  Pstate.HitBank = 0;
  Pstate.AltBank = 0;
  UpdateAddr(PC, phist, ch_i, ch_t[0], ch_t[1]);
  // just do not forget most address are aligned on 4 bytes
  Pstate.alttaken         = getbim(PC);
  Pstate.tage_pred        = Pstate.alttaken;
  Pstate.LongestMatchPred = Pstate.alttaken;
  tage_component_alt      = TAGE_BASE;
  tage_component_tage     = tage_component_alt;

  // Look for the bank with longest matching history
  for (int i = NHIST; i > 0; i--) {
    if (NOSKIP[i] && gtable[i][Pstate.GI[i]].tag == Pstate.GTAG[i]) {
      Pstate.HitBank          = i;
      Pstate.LongestMatchPred = (gtable[Pstate.HitBank][Pstate.GI[Pstate.HitBank]].ctr >= 0);
      break;
    }
  }

  // Look for the alternate bank
  for (int i = Pstate.HitBank - 1; i > 0; i--) {
    if (NOSKIP[i] && gtable[i][Pstate.GI[i]].tag == Pstate.GTAG[i]) {
      Pstate.AltBank = i;
      break;
    }
  }

  // Compute predictions if we found a matching bank
  if (Pstate.HitBank > 0) {
    auto& hit_entry = gtable[Pstate.HitBank][Pstate.GI[Pstate.HitBank]];
    int   hit_conf  = abs(2 * hit_entry.ctr + 1);

    // Handle alternate bank prediction
    if (Pstate.AltBank > 0) {
      auto& alt_entry    = gtable[Pstate.AltBank][Pstate.GI[Pstate.AltBank]];
      Pstate.alttaken    = (alt_entry.ctr >= 0);
      Pstate.AltConf     = (abs(2 * alt_entry.ctr + 1) > 1);
      tage_component_alt = (Pstate.AltBank >= BORN) ? TAGE_LONG : TAGE_SHORT;
    } else {
      Pstate.alttaken = getbim(PC);
    }

    // Choose between longest match and alternate prediction
    bool Huse_alt_on_na = (use_alt_on_na[INDUSEALT] >= 0);
    if (!Huse_alt_on_na || hit_conf > 1) {
      Pstate.tage_pred    = Pstate.LongestMatchPred;
      tage_component_tage = (Pstate.HitBank >= BORN) ? TAGE_LONG : TAGE_SHORT;
    } else {
      Pstate.tage_pred    = Pstate.alttaken;
      tage_component_tage = tage_component_alt;
    }

    // Set confidence levels
    Pstate.HighConf = (hit_conf >= (1 << CWIDTH) - 1);
    Pstate.LowConf  = (hit_conf == 1);
    Pstate.MedConf  = (hit_conf == 5);
  }
}

// compute the prediction
bool TAGE64K::GetPrediction(UINT64 PC, int* bp_confidence, Op* op) {
  if (op->off_path)
    off_path = true;
  else
    off_path = false;

  Tagepred(PC);
  Pstate.pred_taken = Pstate.tage_pred;
  tage_component    = tage_component_tage;

#ifndef SC
  if (!op->off_path)
    STAT_EVENT(op->proc_id, TAGESCL_COMP_TAGE_BASE_CORRECT + op->oracle_info.mispred + tage_component * 2);
  return (tage_pred);
#endif

#ifdef LOOPPREDICTOR
  Pstate.predloop   = getloop(PC);  // loop prediction
  Pstate.pred_taken = ((WITHLOOP >= 0) && (Pstate.LVALID)) ? Pstate.predloop : Pstate.pred_taken;
  tage_component    = ((WITHLOOP >= 0) && (Pstate.LVALID)) ? TAGE_LOOP : tage_component_tage;
#endif
  Pstate.pred_inter    = Pstate.pred_taken;
  tage_component_inter = tage_component;

  // Compute the SC prediction
  Pstate.LSUM = 0;
  // integrate BIAS prediction
  int8_t ctr = Bias[INDBIAS];

  Pstate.LSUM += (2 * ctr + 1);
  ctr = BiasSK[INDBIASSK];
  Pstate.LSUM += (2 * ctr + 1);
  ctr = BiasBank[INDBIASBANK];
  Pstate.LSUM += (2 * ctr + 1);
#ifdef VARTHRES
  Pstate.LSUM = (1 + (WB[INDUPDS] >= 0)) * Pstate.LSUM;
#endif
  // integrate the GEHL predictions
  Pstate.LSUM += Gpredict((PC << 1) + Pstate.pred_inter, GHIST, Gm, GGEHL, GNB, LOGGNB, WG);
  Pstate.LSUM += Gpredict(PC, phist, Pm, PGEHL, PNB, LOGPNB, WP);
#ifdef LOCALH
  Pstate.LSUM += Gpredict(PC, L_shist[INDLOCAL], Lm, LGEHL, LNB, LOGLNB, WL);
#endif
#ifdef LOCALS
  Pstate.LSUM += Gpredict(PC, S_slhist[INDSLOCAL], Sm, SGEHL, SNB, LOGSNB, WS);
#endif
#ifdef LOCALT
  Pstate.LSUM += Gpredict(PC, T_slhist[INDTLOCAL], Tm, TGEHL, TNB, LOGTNB, WT);
#endif

#ifdef IMLI
  Pstate.LSUM += Gpredict(PC, IMHIST[(IMLIcount)], IMm, IMGEHL, IMNB, LOGIMNB, WIM);
  Pstate.LSUM += Gpredict(PC, IMLIcount, Im, IGEHL, INB, LOGINB, WI);
#endif
  bool SCPRED = (Pstate.LSUM >= 0);
  // just  an heuristic if the respective contribution of component groups can be multiplied by 2 or not
  Pstate.THRES = (updatethreshold >> 3) + Pupdatethreshold[INDUPD]
#ifdef VARTHRES
      + 12 * ((WB[INDUPDS] >= 0) + (WP[INDUPDS] >= 0)
#ifdef LOCALH
                 + (WS[INDUPDS] >= 0) + (WT[INDUPDS] >= 0) + (WL[INDUPDS] >= 0)
#endif
                 + (WG[INDUPDS] >= 0)
#ifdef IMLI
                 + (WI[INDUPDS] >= 0)
#endif
             )
#endif
      ;

  // Minimal benefit in trying to avoid accuracy loss on low confidence SC prediction and  high/medium confidence on TAGE
  //  but just uses 2 counters 0.3 % MPKI reduction
  //  Handle disagreement between TAGE and SC predictions
  if (Pstate.pred_inter != SCPRED) {
    int abs_lsum      = abs(Pstate.LSUM);
    Pstate.pred_taken = SCPRED;
    tage_component    = TAGE_SC;
    // High confidence case
    if (Pstate.HighConf) {
      if (abs_lsum < Pstate.THRES / 4) {
        // Use TAGE prediction when |LSUM| is much smaller than threshold
        Pstate.pred_taken = Pstate.pred_inter;
        tage_component    = tage_component_inter;
      } else if (abs_lsum < Pstate.THRES / 2) {
        // Use SecondH to choose between SC and TAGE when |LSUM| is moderately small
        bool use_sc       = (SecondH < 0);
        Pstate.pred_taken = use_sc ? SCPRED : Pstate.pred_inter;
        tage_component    = use_sc ? TAGE_SC : tage_component_inter;
        if (Pstate.pred_taken == SCPRED && !op->off_path)
          STAT_EVENT(op->proc_id, TAGESCL_SC_SMALLLSUM + 2);  // SC from HighConf
      }
    }
    // Medium confidence case
    if (Pstate.MedConf && abs_lsum < Pstate.THRES / 4) {
      // Use FirstH to choose between SC and TAGE
      bool use_sc       = (FirstH < 0);
      Pstate.pred_taken = use_sc ? SCPRED : Pstate.pred_inter;
      tage_component    = use_sc ? TAGE_SC : tage_component_inter;
      if (Pstate.pred_taken == SCPRED && !op->off_path)
        STAT_EVENT(op->proc_id, TAGESCL_SC_SMALLLSUM + 1);  // SC from MedConf
    }
    // Low confidence case
    if (!Pstate.MedConf && !Pstate.HighConf && Pstate.pred_taken == SCPRED && !op->off_path)
      STAT_EVENT(op->proc_id, TAGESCL_SC_SMALLLSUM);  // Exclusive SC
  } else if (!op->off_path)                           // TAGE and SC predictions agree
    STAT_EVENT(op->proc_id, TAGESCL_SC_SMALLLSUM + 3);
  if (!op->off_path) {
    STAT_EVENT(op->proc_id, TAGESCL_COMP_TAGE_BASE_CORRECT + (Pstate.pred_taken != op->oracle_info.dir) + tage_component * 2);
    STAT_EVENT(op->proc_id, TAGESCL_COMP_BTB_HIT_TAGE_BASE_CORRECT + (Pstate.pred_taken != op->oracle_info.dir) + tage_component * 2 + op->oracle_info.btb_miss * 12);
  }

  *bp_confidence = 0;
  if (Pstate.HighConf) *bp_confidence += 3;
  if (Pstate.MedConf) *bp_confidence += 2;
  if (Pstate.LowConf) *bp_confidence += 1;
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

void TAGE64K::HistoryUpdate(UINT64 PC, OpType opType, bool taken,
    UINT64 target, int hitbank) {
  assert(!off_path);

  int brtype   = GetBrtypeFromOptype(opType);
  int T        = (PC ^ (PC >> 7) ^ (PC >> 13)) ^ (taken << 5);       // Wider shifts, move taken bit
  int PATH     = (PC ^ (PC >> 5) ^ (PC >> 11)) ^ (brtype << 3);      // Add branch type info
  int hit_info = (hitbank << 4) ^ (hitbank >> 2) ^ (hitbank * 0x7);  // More mixing

  if ((brtype == 3) && taken) {
    T ^= (target >> 7) ^ (target >> 13);     // Match T's shift pattern
    PATH ^= (target >> 5) ^ (target >> 11);  // Match PATH's shift pattern
  }

  int maxt = ((brtype & 2) != 0) ? 3 : 2;

  for (int t = 0; t < maxt; t++) {
    bool DIR = (T & 1);
    T >>= 1;
    // Take bits from T, PATH, and hit_info
    int PATHBIT = (PATH & 0xFF);  // Use lower bits of PATH for variation
    PATH >>= 1;                   // Shift PATH for next iteration
    // Enhanced condpattern update for better MYRANDOM distribution
    condpattern = (condpattern << 3) ^ DIR;               // Larger shift for DIR
    condpattern = (condpattern << 3) ^ (PATHBIT & 0x7F);  // Use 7 bits of PATH
    condpattern = (condpattern << 2) ^ (hit_info & 3);    // Use 2 bits of hit_info
    // Mix bits to improve randomness for different masks (1,31,127)
    condpattern = (condpattern ^ (condpattern >> 4)) & 0x7fffffff;
    hit_info >>= 1;
  }

#ifdef IMLI
  if (brtype & 1) {
    IMHIST[IMLIcount] = (IMHIST[IMLIcount] << 1) + taken;
    // Check if this is a backward branch (potential loop)
    if (target < PC) {
      // Reset IMLI counter when exiting the innermost loop
      if (!taken)
        IMLIcount = 0;
      else if (IMLIcount < ((1 << Im[0]) - 1))  // Limited by Im[0] bits to prevent overflow
        IMLIcount++;                            // Increment IMLI counter for each iteration of the innermost loop
    }
  }
#endif

  if (brtype & 1) {
    L_shist[INDLOCAL]   = (L_shist[INDLOCAL] << 1) + (taken);
    S_slhist[INDSLOCAL] = ((S_slhist[INDSLOCAL] << 1) + taken) ^ (PC & 15);
    T_slhist[INDTLOCAL] = (T_slhist[INDTLOCAL] << 1) + taken;
  }
  // END UPDATE  HISTORIES
}

void TAGE64K::SpecUpdateAtCond(UINT64 PC, bool dir, bool pred) {
#ifdef LOOPPREDICTOR
  SpecLoopUpdate(PC, dir);
#endif
  bool SCPRED = (Pstate.LSUM >= 0);
  if ((SCPRED != dir) || ((abs(Pstate.LSUM) < Pstate.THRES))) {
    Gupdate((PC << 1) + Pstate.pred_inter, dir, GHIST, Gm, GGEHL, GNB, LOGGNB, WG, Pstate.LSUM);
    Gupdate(PC, dir, phist, Pm, PGEHL, PNB, LOGPNB, WP, Pstate.LSUM);
  }
}

void TAGE64K::SpecUpdate(UINT64 PC, OpType opType, bool dir, bool pred, UINT64 target) {
  int brtype = GetBrtypeFromOptype(opType);
  GlobalStateUpdate(PC, target, brtype, dir);
}

void TAGE64K::GlobalStateUpdate(UINT64 PC, UINT64 br_target, int brtype, bool pred_dir) {
  if (brtype & 1)
    GHIST = (GHIST << 1) + (pred_dir & (br_target < PC));

  int T    = ((PC ^ (PC >> 2))) ^ pred_dir;
  int PATH = PC ^ (PC >> 2) ^ (PC >> 4);
  // if branch is indirect, encode global history with 3 bits. Otherwise, 2 bits
  int maxt = ((brtype & 2) != 0) ? 3 : 2;

  if ((brtype == 3) & pred_dir) {
    T    = (T ^ (br_target >> 2));
    PATH = PATH ^ (br_target >> 2) ^ (br_target >> 4);
  }

  for (int t = 0; t < maxt; t++) {
    bool DIR = (T & 1);
    T >>= 1;
    int PATHBIT = (PATH & 127);
    PATH >>= 1;
    // update  history
    ptghist--;  // pointer of global history
    ghist[ptghist & (HISTBUFFERLENGTH - 1)] = DIR;
    phist                                   = (phist << 1) ^ PATHBIT;

    for (int i = 1; i <= NHIST; i++) {
      ch_i[i].update(ghist, ptghist);
      ch_t[0][i].update(ghist, ptghist);
      ch_t[1][i].update(ghist, ptghist);
    }
  }
  phist = (phist & ((1 << PHISTWIDTH) - 1));  // path history
}

// PREDICTOR UPDATE

void TAGE64K::UpdatePredictor(UINT64 PC, OpType opType, bool resolveDir,
    bool predDir, UINT64 branchTarget, Counter op_num, const PredictorStates& state) {
  assert(!off_path);
#ifdef LOOPPREDICTOR
  if (state.LVALID)
    if (state.pred_taken != state.predloop)
      ctrupdate(WITHLOOP, (state.predloop == resolveDir), 7);
  LoopUpdate(PC, resolveDir, (state.pred_taken != resolveDir), op_num, state.LHIT);
#endif

#ifdef SC
  // Calculate index values for bias tables
  int IndBIAS = (((((PC ^ (PC >> 2)) << 1) ^
                      (state.LowConf & (state.LongestMatchPred != state.alttaken)))
                     << 1) +
                    state.pred_inter) &
      ((1 << LOGBIAS) - 1);

  int IndBIASSK = (((((PC ^ (PC >> (LOGBIAS - 2))) << 1) ^
                        (state.HighConf))
                       << 1) +
                      state.pred_inter) &
      ((1 << LOGBIAS) - 1);

  int IndBIASBANK = (state.pred_inter +
                        (((state.HitBank + 1) / 4) << 4) +
                        (state.HighConf << 1) +
                        (state.LowConf << 2) +
                        ((state.AltBank != 0) << 3) +
                        ((PC ^ (PC >> 2)) << 7)) &
      ((1 << LOGBIAS) - 1);

  int IndUSEALT = ((((state.HitBank - 1) / 8) << 1) + state.AltConf) % (SIZEUSEALT - 1);

  int  abs_lsum = abs(state.LSUM);
  bool SCPRED   = (state.LSUM >= 0);
  if (state.pred_inter != SCPRED) {
    // High confidence case
    if (state.HighConf && abs_lsum < state.THRES)
      if (abs_lsum >= state.THRES / 4 && abs_lsum < state.THRES / 2)
        ctrupdate(SecondH, (state.pred_inter == resolveDir), CONFWIDTH);
    // Medium confidence case
    if (state.MedConf && abs_lsum < state.THRES / 4)
      ctrupdate(FirstH, (state.pred_inter == resolveDir), CONFWIDTH);
  }

  if ((SCPRED != resolveDir) || (abs_lsum < state.THRES)) {
    // Update prediction thresholds
    if (SCPRED != resolveDir) {
      Pupdatethreshold[INDUPD]++;
      updatethreshold++;
    } else {
      Pupdatethreshold[INDUPD]--;
      updatethreshold--;
    }

    // Clamp Pupdatethreshold within bounds
    int max_thresh_p         = (1 << (WIDTHRESP - 1)) - 1;
    int min_thresh_p         = -(1 << (WIDTHRESP - 1));
    Pupdatethreshold[INDUPD] = std::min(std::max(Pupdatethreshold[INDUPD], min_thresh_p), max_thresh_p);

    // Clamp updatethreshold within bounds
    int max_thresh  = (1 << (WIDTHRES - 1)) - 1;
    int min_thresh  = -(1 << (WIDTHRES - 1));
    updatethreshold = std::min(std::max(updatethreshold, min_thresh), max_thresh);

#ifdef VARTHRES
    // Calculate bias sum
    int bias_sum = (2 * Bias[IndBIAS] + 1) +
        (2 * BiasSK[IndBIASSK] + 1) +
        (2 * BiasBank[IndBIASBANK] + 1);

    // Calculate adjusted sum
    int XSUM = state.LSUM - ((WB[INDUPD] >= 0) * bias_sum);

    // Update weight if sign changes after adding bias
    if ((XSUM + bias_sum >= 0) != (XSUM >= 0))
      ctrupdate(WB[INDUPDS], (bias_sum >= 0) == resolveDir, EWIDTH);
#endif
    // Update bias counters
    ctrupdate(Bias[IndBIAS], resolveDir, PERCWIDTH);
    ctrupdate(BiasSK[IndBIASSK], resolveDir, PERCWIDTH);
    ctrupdate(BiasBank[IndBIASBANK], resolveDir, PERCWIDTH);

#ifdef LOCALH
    // Update local history predictors
    Gupdate(PC, resolveDir, L_shist[INDLOCAL], Lm, LGEHL, LNB, LOGLNB, WL, state.LSUM);
#endif
#ifdef LOCALS
    Gupdate(PC, resolveDir, S_slhist[INDSLOCAL], Sm, SGEHL, SNB, LOGSNB, WS, state.LSUM);
#endif
#ifdef LOCALT
    Gupdate(PC, resolveDir, T_slhist[INDTLOCAL], Tm, TGEHL, TNB, LOGTNB, WT, state.LSUM);
#endif

#ifdef IMLI
    // Update IMLI predictors
    Gupdate(PC, resolveDir, IMHIST[IMLIcount], IMm, IMGEHL, IMNB, LOGIMNB, WIM, state.LSUM);
    Gupdate(PC, resolveDir, IMLIcount, Im, IGEHL, INB, LOGINB, WI, state.LSUM);
#endif
  }
#endif

  // TAGE UPDATE
  bool ALLOC = (state.tage_pred != resolveDir) && (state.HitBank < NHIST);

  // Get correct indices for hit and alt banks
  int hit_idx = USE_PRED_STATES ? (state.HitBank > 0 ? noskip_index[state.HitBank] : 0) : state.HitBank;
  int alt_idx = USE_PRED_STATES ? (state.AltBank > 0 ? noskip_index[state.AltBank] : 0) : state.AltBank;
  ASSERTM(0, hit_idx >= 0, "noskip_index[state.HitBank] is negative: %d\n", hit_idx);
  ASSERTM(0, alt_idx >= 0, "noskip_index[state.AltBank] is negative: %d\n", alt_idx);
  // Create a dummy entry for when bank is 0, use static to avoid recreating it every time
  static cbp64_gentry dummy_entry;

  // References with proper initialization
  cbp64_gentry& hit_entry = (state.HitBank > 0) ? gtable[state.HitBank][state.GI[hit_idx]] : dummy_entry;

  cbp64_gentry& alt_entry = (state.AltBank > 0) ? gtable[state.AltBank][state.GI[alt_idx]] : dummy_entry;

  // Handle hit bank allocation
  if (state.HitBank > 0) {
    // Check if entry is pseudo-newly allocated (weak prediction counter)
    bool PseudoNewAlloc = (abs(2 * hit_entry.ctr + 1) <= 1);
    if (PseudoNewAlloc) {
      // Don't allocate if longest match was correct
      if (state.LongestMatchPred == resolveDir)
        ALLOC = false;
      // Update alternate predictor if needed
      if (state.LongestMatchPred != state.alttaken)
        ctrupdate(use_alt_on_na[IndUSEALT], (state.alttaken == resolveDir), ALTWIDTH);
    }
  }

  // Reduce allocation frequency on correct predictions
  if (state.pred_taken == resolveDir && (MYRANDOM(op_num) & 31) != 0)
    ALLOC = false;

  if (ALLOC) {
    int T       = NNN;
    int A       = ((MYRANDOM(op_num) & 127) < 32) ? 2 : 1;
    int Penalty = 0;
    int NA      = 0;
    int DEP     = ((state.HitBank - 1 + 2 * A) & 0xffe) ^ (MYRANDOM(op_num) & 1);

    for (int I = DEP; I < NHIST && T > 0; I += 2) {
      // Try primary entry
      int  i    = I + 1;
      bool Done = false;
      if (NOSKIP[i]) {
        // Get correct index based on USE_PRED_STATES
        int idx = USE_PRED_STATES ? noskip_index[i] : i;
        assert(idx >= 0);
        auto& entry = gtable[i][state.GI[idx]];

        if (entry.u == 0) {
#ifdef OPTREMP
          if (abs(2 * entry.ctr + 1) <= 3) {
#endif
            entry.tag = state.GTAG[idx];
            entry.ctr = resolveDir ? 0 : -1;
            NA++;
            if (T <= 0) break;
            I += 2;
            Done = true;
            T--;
#ifdef OPTREMP
          } else {
            entry.ctr += (entry.ctr > 0) ? -1 : 1;
          }
#endif
        } else {
          Penalty++;
        }
      }

      // Try alternate entry if primary failed
      if (!Done) {
        i = (I ^ 1) + 1;
        if (NOSKIP[i]) {
          // Get correct index based on USE_PRED_STATES
          int idx = USE_PRED_STATES ? noskip_index[i] : i;
          assert(idx >= 0);
          auto& entry = gtable[i][state.GI[idx]];

          if (entry.u == 0) {
#ifdef OPTREMP
            if (abs(2 * entry.ctr + 1) <= 3) {
#endif
              entry.tag = state.GTAG[idx];
              entry.ctr = resolveDir ? 0 : -1;
              NA++;
              if (T <= 0) break;
              I += 2;
              T--;
#ifdef OPTREMP
            } else {
              entry.ctr += (entry.ctr > 0) ? -1 : 1;
            }
#endif
          } else {
            Penalty++;
          }
        }
      }
    }

    TICK += (Penalty - 2 * NA);
    TICK = std::max(0, TICK);
    if (TICK >= BORNTICK) {
      for (int i = 1; i <= BORN; i += BORN - 1) {
        for (int j = 0; j < SizeTable[i]; j++) {
          gtable[i][j].u >>= 1;
        }
      }
      TICK = 0;
    }
  }

  if (state.HitBank > 0) {
    bool weak_counter = (abs(2 * hit_entry.ctr + 1) == 1);
    // Update predictions when hit counter is weak
    if (weak_counter && state.LongestMatchPred != resolveDir) {
      // Update alternate prediction as protection
      if (state.AltBank > 0)
        ctrupdate(alt_entry.ctr, resolveDir, CWIDTH);
      else
        baseupdate(resolveDir, PC, btable);
    }
    // Update hit bank prediction
    ctrupdate(hit_entry.ctr, resolveDir, CWIDTH);
    // Reset usefulness on weak counter
    if (weak_counter)
      hit_entry.u = 0;
  }

  // Special case: update usefulness when alternate prediction was correct
  if (state.alttaken == resolveDir && state.AltBank > 0) {
    bool alt_strong = (abs(2 * alt_entry.ctr + 1) == 7);
    if (alt_strong && state.HitBank > 0 && hit_entry.u == 1 && state.LongestMatchPred == resolveDir)
      hit_entry.u = 0;
  } else
    baseupdate(resolveDir, PC, btable);

  // Increment usefulness on correct longest match prediction
  if (state.LongestMatchPred != state.alttaken && state.LongestMatchPred == resolveDir)
    if (hit_entry.u < (1 << UWIDTH) - 1)
      hit_entry.u++;
  // END TAGE UPDATE

  HistoryUpdate(PC, opType, resolveDir, branchTarget, state.HitBank);
  // END PREDICTOR UPDATE
}

void TAGE64K::UpdateAddr(UINT64 PC, long long path_history, cbp64_folded_history* index, cbp64_folded_history* tag0, cbp64_folded_history* tag1) {
  for (int i = 1; i <= NHIST; i += 2) {
    Pstate.GI[i]       = gindex(PC, i, path_history, index);
    Pstate.GTAG[i]     = gtag(PC, i, tag0, tag1);
    Pstate.GTAG[i + 1] = Pstate.GTAG[i];
    Pstate.GI[i + 1]   = Pstate.GI[i] ^ (Pstate.GTAG[i] & ((1 << LOGG) - 1));
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

void TAGE64K::NonSpecUpdateAtCond(UINT64 PC, OpType opType, bool resolveDir,
    bool predDir, UINT64 branchTarget, Counter key, Counter op_num) {
  if (USE_PRED_STATES) {
    auto& key_index = predictor_states.get<0>();
    auto  pit       = key_index.find(key);
    assert(pit != key_index.end());
    auto& mutable_state = const_cast<PredictorStates&>(pit->state);
    UpdatePredictor(PC, opType, resolveDir, predDir, branchTarget, op_num, mutable_state);
  } else {
    UpdatePredictor(PC, opType, resolveDir, predDir, branchTarget, op_num, Pstate);
  }
}

#define GINDEX (((long long) PC) ^ bhist ^ (bhist >> (8 - i)) ^ (bhist >> (16 - 2 * i)) ^ (bhist >> (24 - 3 * i)) ^ (bhist >> (32 - 3 * i)) ^ (bhist >> (40 - 4 * i))) & ((1 << (logs - (i >= (NBR - 2)))) - 1)
int TAGE64K::Gpredict(UINT64 PC, long long BHIST, int* length,
    int8_t** tab, int NBR, int logs, int8_t* W) {
  int PERCSUM = 0;
  for (int i = 0; i < NBR; i++) {
    long long bhist = BHIST & ((long long) ((1 << length[i]) - 1));
    long long index = GINDEX;
    int8_t    ctr   = tab[i][index];
    PERCSUM += (2 * ctr + 1);
  }
#ifdef VARTHRES
  PERCSUM = (1 + (W[INDUPDS] >= 0)) * PERCSUM;
#endif
  return ((PERCSUM));
}

void TAGE64K::Gupdate(UINT64 PC, bool taken, long long BHIST, int* length,
    int8_t** tab, int NBR, int logs, int8_t* W, int LSUM) {
  int PERCSUM = 0;
  for (int i = 0; i < NBR; i++) {
    long long bhist = BHIST & ((long long) ((1 << length[i]) - 1));
    long long index = GINDEX;
    PERCSUM += (2 * tab[i][index] + 1);
    ctrupdate(tab[i][index], taken, PERCWIDTH);
  }
#ifdef VARTHRES
  int XSUM = LSUM - ((W[INDUPDS] >= 0)) * PERCSUM;
  if ((XSUM + PERCSUM >= 0) != (XSUM >= 0))
    ctrupdate(W[INDUPDS], ((PERCSUM >= 0) == taken), EWIDTH);
#endif
}

void TAGE64K::TrackOtherInst(UINT64 PC, OpType opType, bool taken,
    UINT64 branchTarget, Counter key) {
  if (USE_PRED_STATES) {
    auto& key_index = predictor_states.get<0>();
    auto  pit       = key_index.find(key);
    assert(pit != key_index.end());
    HistoryUpdate(PC, opType, taken, branchTarget, pit->state.HitBank);
  } else
    HistoryUpdate(PC, opType, taken, branchTarget, Pstate.HitBank);
}

void TAGE64K::SavePredictorStates(UINT64 PC, Counter key, uns proc_id) {
  assert(!off_path);
  auto& key_index = predictor_states.get<0>();  // Get the key index
  auto  it        = key_index.find(key);
  assert(it == key_index.end());

  PredictorStates state(true);
// loop
#ifdef LOOPPREDICTOR
  state.LHIT     = Pstate.LHIT;
  state.LVALID   = Pstate.LVALID;
  state.predloop = Pstate.predloop;
#endif
  // SC + TAGE
  state.tage_pred        = Pstate.tage_pred;
  state.pred_inter       = Pstate.pred_inter;
  state.pred_taken       = Pstate.pred_taken;
  state.LongestMatchPred = Pstate.LongestMatchPred;
  state.alttaken         = Pstate.alttaken;
  state.HighConf         = Pstate.HighConf;
  state.MedConf          = Pstate.MedConf;
  state.LowConf          = Pstate.LowConf;
  state.AltConf          = Pstate.AltConf;
  state.THRES            = Pstate.THRES;
  state.LSUM             = Pstate.LSUM;
  state.HitBank          = Pstate.HitBank;
  state.AltBank          = Pstate.AltBank;
  assert(NOSKIP[state.HitBank] || state.HitBank == 0);  // HitBank should be valid or 0
  assert(NOSKIP[state.AltBank] || state.AltBank == 0);
  int8_t j = 0;
  for (int i = 0; i <= NHIST; i++) {
    if (NOSKIP[i]) {
      state.GI[j]   = Pstate.GI[i];
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

void TAGE64K::RestorePredictorstates(Counter key, UINT64 PC) {
  auto& key_pindex = predictor_states.get<0>();
  auto  pit        = key_pindex.find(key);
  assert(pit != key_pindex.end());

// loop
#ifdef LOOPPREDICTOR
  Pstate.LHIT     = pit->state.LHIT;
  Pstate.LVALID   = pit->state.LVALID;
  Pstate.predloop = pit->state.predloop;
#endif
  // // SC + TAGE
  Pstate.tage_pred        = pit->state.tage_pred;
  Pstate.pred_inter       = pit->state.pred_inter;
  Pstate.pred_taken       = pit->state.pred_taken;
  Pstate.LongestMatchPred = pit->state.LongestMatchPred;
  Pstate.alttaken         = pit->state.alttaken;
  Pstate.HighConf         = pit->state.HighConf;
  Pstate.MedConf          = pit->state.MedConf;
  Pstate.LowConf          = pit->state.LowConf;
  Pstate.AltConf          = pit->state.AltConf;
  Pstate.THRES            = pit->state.THRES;
  Pstate.LSUM             = pit->state.LSUM;
  Pstate.HitBank          = pit->state.HitBank;
  Pstate.AltBank          = pit->state.AltBank;
  // store only elements with NOSKIP
  int j = 0;
  for (int i = 0; i <= NHIST; i++) {
    if (NOSKIP[i]) {
      assert(noskip_index[i] >= 0);
      Pstate.GI[i]   = pit->state.GI[j];
      Pstate.GTAG[i] = pit->state.GTAG[j];
      j++;
    }
  }
  off_path = false;
}

void TAGE64K::TakeCheckpoint(UINT64 PC, Counter key, OpType opType, Cf_Type cftype) {
  auto& key_index = checkpoints.get<0>();  // Get the key index
  auto  it        = key_index.find(key);
  assert(it == key_index.end());
  long unsigned int org_size = checkpoints.size();
  Checkpoint        state;
  for (size_t i = 0; i < NHIST + 1; ++i) {
    state.bch_i[i].comp        = ch_i[i].comp;
    state.bch_i[i].CLENGTH     = ch_i[i].CLENGTH;
    state.bch_i[i].OLENGTH     = ch_i[i].OLENGTH;
    state.bch_i[i].OUTPOINT    = ch_i[i].OUTPOINT;
    state.bch_t[0][i].comp     = ch_t[0][i].comp;
    state.bch_t[0][i].CLENGTH  = ch_t[0][i].CLENGTH;
    state.bch_t[0][i].OLENGTH  = ch_t[0][i].OLENGTH;
    state.bch_t[0][i].OUTPOINT = ch_t[0][i].OUTPOINT;
    state.bch_t[1][i].comp     = ch_t[1][i].comp;
    state.bch_t[1][i].CLENGTH  = ch_t[1][i].CLENGTH;
    state.bch_t[1][i].OLENGTH  = ch_t[1][i].OLENGTH;
    state.bch_t[1][i].OUTPOINT = ch_t[1][i].OUTPOINT;
  }
  state.ptghist = ptghist;
  state.GHIST   = GHIST;
  state.phist   = phist;
#ifdef LOOPPREDICTOR
  std::copy(ltable, ltable + (1 << LOGL), state.ldata);
#endif
  for (int i = 0; i < GNB; ++i)
    std::copy(GGEHL[i], GGEHL[i] + (1 << LOGGNB), state.GGEHL[i]);
  for (int i = 0; i < PNB; ++i)
    std::copy(PGEHL[i], PGEHL[i] + (1 << LOGPNB), state.PGEHL[i]);
  std::copy(WG, WG + (1 << 3), state.WG);
  std::copy(WP, WP + (1 << 3), state.WP);
  checkpoints.insert(CheckpointEntry(key, state));
  assert(checkpoints.size() == (org_size + 1));
}

void TAGE64K::VerifyCheckpoint(UINT64 PC, Counter key, uns proc_id) {
  auto& key_index = checkpoints.get<0>();  // Get the key index
  auto  it        = key_index.find(key);
  assert(it != key_index.end());
  // predictor states
}

void TAGE64K::RepairStateAndUpdate(UINT64 PC, bool new_dir, UINT64 branchTarget, Recovery_Info* recovery_info) {
  // recover bp states
  Counter key = recovery_info->branch_id;
  if (SPEC_LEVEL < BP_PRED_ON_SPEC_UPDATE_S_ONOFF_N_ON)
    VerifyCheckpoint(PC, key, recovery_info->proc_id);
  RestoreCheckpoint(PC, key, recovery_info->proc_id);
}

void TAGE64K::RestoreCheckpoint(UINT64 PC, Counter key, uns proc_id) {
  auto& key_index = checkpoints.get<0>();
  auto  it        = key_index.find(key);
  assert(it != key_index.end());  // Ensure the checkpoint exists
  if (!(SPEC_LEVEL < BP_PRED_ON_SPEC_UPDATE_S_ONOFF_N_ON)) {
    for (size_t i = 0; i < NHIST + 1; ++i) {
      ch_i[i].comp        = it->state.bch_i[i].comp;
      ch_i[i].CLENGTH     = it->state.bch_i[i].CLENGTH;
      ch_i[i].OLENGTH     = it->state.bch_i[i].OLENGTH;
      ch_i[i].OUTPOINT    = it->state.bch_i[i].OUTPOINT;
      ch_t[0][i].comp     = it->state.bch_t[0][i].comp;
      ch_t[0][i].CLENGTH  = it->state.bch_t[0][i].CLENGTH;
      ch_t[0][i].OLENGTH  = it->state.bch_t[0][i].OLENGTH;
      ch_t[0][i].OUTPOINT = it->state.bch_t[0][i].OUTPOINT;
      ch_t[1][i].comp     = it->state.bch_t[1][i].comp;
      ch_t[1][i].CLENGTH  = it->state.bch_t[1][i].CLENGTH;
      ch_t[1][i].OLENGTH  = it->state.bch_t[1][i].OLENGTH;
      ch_t[1][i].OUTPOINT = it->state.bch_t[1][i].OUTPOINT;
    }
    for (int i = 0; i < GNB; ++i) {
      std::copy(it->state.GGEHL[i], it->state.GGEHL[i] + (1 << LOGGNB), GGEHL[i]);
    }
    for (int i = 0; i < PNB; ++i) {
      std::copy(it->state.PGEHL[i], it->state.PGEHL[i] + (1 << LOGPNB), PGEHL[i]);
    }
    std::copy(it->state.WG, it->state.WG + (1 << 3), WG);
    std::copy(it->state.WP, it->state.WP + (1 << 3), WP);

    GHIST   = it->state.GHIST;
    phist   = it->state.phist;
    ptghist = it->state.ptghist;
#ifdef LOOPPREDICTOR
    std::copy(it->state.ldata, it->state.ldata + (1 << LOGL), ltable);
#endif
    RestorePredictorstates(key, PC);
  }
  auto& key_pindex = predictor_states.get<0>();
  auto  pit        = key_pindex.upper_bound(key);
  it               = key_index.upper_bound(key);
  if (pit != key_pindex.end())
    key_pindex.erase(pit, key_pindex.end());
  if (it != key_index.end())
    key_index.erase(it, key_index.end());
}

void TAGE64K::RetireCheckpoint(Counter key) {
  auto& key_index  = checkpoints.get<0>();
  auto& key_pindex = predictor_states.get<0>();
  auto  it         = key_index.find(key);
  auto  pit        = key_pindex.find(key);
  // assert(it != key_index.end()); // key can be not exist
  assert(pit != key_pindex.end());  // Ensure the checkpoint exists
  if (it != key_index.end())
    key_index.erase(it);
  if (pit != key_pindex.end())
    key_pindex.erase(pit);
}

#ifdef LOOPPREDICTOR
int TAGE64K::lindex(UINT64 PC) {
  return (((PC ^ (PC >> 2)) & ((1 << (LOGL - 2)) - 1)) << 2);
}

int TAGE64K::lib(UINT64 PC) {
  return ((PC >> (LOGL - 2)) & ((1 << (LOGL - 2)) - 1));
}

int TAGE64K::ltag(UINT64 PC) {
  int ltag = (PC >> (LOGL - 2)) & ((1 << 2 * LOOPTAG) - 1);
  ltag ^= (ltag >> LOOPTAG);
  return (ltag & ((1 << LOOPTAG) - 1));
}

// loop prediction: only used if high confidence
// skewed associative 4-way
// At fetch time: speculative
#define CONFLOOP 15
bool TAGE64K::getloop(UINT64 PC) {
  // Initialize loop predictor state
  Pstate.LHIT = -1;
  LI          = lindex(PC);  // Get loop index
  LIB         = lib(PC);     // Get loop index bits
  LTAG        = ltag(PC);    // Get loop tag

  // Search through 4-way skewed associative structure
  for (int i = 0; i < 4; i++) {
    // Calculate index with XOR-based hash to reduce conflicts
    int   index = (LI ^ ((LIB >> i) << 2)) + i;
    auto& entry = ltable[index];
    // Check if we have a tag match
    if (entry.TAG == LTAG) {
      Pstate.LHIT = i;  // Record which way matched
      // Entry is valid if either:
      // 1. Maximum confidence reached (CONFLOOP), or
      // 2. Confidence * iterations > 128 (empirical threshold)
      Pstate.LVALID = (entry.confid == CONFLOOP) ||
          (entry.confid * entry.NbIter > 128);
      // If we're at the last iteration, predict the opposite
      // of the loop direction (loop exit prediction)
      if (entry.CurrentIter + 1 == entry.NbIter)
        return !entry.dir;
      // Otherwise predict the loop direction
      return entry.dir;
    }
  }
  // No matching entry found
  Pstate.LVALID = false;
  return false;
}

void TAGE64K::SpecLoopUpdate(UINT64 PC, bool Taken) {
  if (Pstate.LHIT < 0) return;
  // Calculate index into loop predictor table
  int   index = (LI ^ ((LIB >> Pstate.LHIT) << 2)) + Pstate.LHIT;
  auto& entry = ltable[index];

  // Handle valid loop prediction
  if (Pstate.LVALID) {
    if (Taken != Pstate.predloop) {
      // Free the entry on misprediction
      entry.NbIter      = 0;
      entry.age         = 0;
      entry.confid      = 0;
      entry.CurrentIter = 0;
      return;
    }
    // Increment age when prediction differs from TAGE or randomly
    if ((Pstate.predloop != Pstate.tage_pred) || ((Seed & 7) == 0))
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
        entry.dir    = Taken;
        entry.NbIter = 0;
        entry.age    = 0;
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

void TAGE64K::LoopUpdate(UINT64 PC, bool Taken, bool ALLOC, Counter op_num, int lhit) {
  if (lhit >= 0) {
    MYRANDOM(op_num);
    return;
  }
  if (!ALLOC)
    return;

  UINT64 X = MYRANDOM(op_num) & 3;
  // 25% chance to attempt allocation
  if ((MYRANDOM(op_num) & 3) != 0) return;
  int LHIT  = X & 3;
  int index = (LI ^ ((LIB >> LHIT) << 2)) + LHIT;
  if (ltable[index].age == 0) {
    ltable[index].dir = !Taken;
    // most of mispredictions are on last iterations
    ltable[index].TAG         = LTAG;
    ltable[index].NbIter      = 0;
    ltable[index].age         = 7;
    ltable[index].confid      = 0;
    ltable[index].CurrentIter = 0;
  } else
    ltable[index].age--;
}

#endif

TAGE64K2::TAGE64K2(void) : TAGE64K() {}

TAGE64K2& TAGE64K2::operator=(const TAGE64K& other) {
  TAGE64K::operator=(other);
  return *this;
}
