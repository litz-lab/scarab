#ifndef _TAGE64K_H_
#define _TAGE64K_H_

#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "bp.param.h"

#include "cbp_to_scarab.h"
// #include "bt9.h"
// #include "bt9_reader.h"

#define BORNTICK 1024
// To get the predictor storage budget on stderr  uncomment the next line
#define PRINTSIZE
#include <algorithm>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>
#include <memory>
#include <sstream>
#include <vector>

// #define SC    // 8.2 % if TAGE alone
#define IMLI  // 0.2 %
#define LOCALH

#ifdef LOCALH          // 2.7 %
// #define LOOPPREDICTOR  // loop predictor enable
#define LOCALS         // enable the 2nd local history
#define LOCALT         // enables the 3rd local history

#endif

#define CONFWIDTH 7  // for the counters in the choser
#define HISTBUFFERLENGTH \
  4096  // we use a 4K entries history buffer to store the branch history (this allows us to explore using history
        // length up to 4K)

// utility class for index computation
// this is the cyclic shift register for folding
// a long global history into a smaller number of bits; see P. Michaud's PPM-like predictor at CBP-1
class cbp64_folded_history {
 public:
  unsigned comp;
  int CLENGTH;
  int OLENGTH;
  int OUTPOINT;

  cbp64_folded_history() {}

  void init(int original_length, int compressed_length) {
    comp = 0;
    OLENGTH = original_length;
    CLENGTH = compressed_length;
    OUTPOINT = OLENGTH % CLENGTH;
  }

  void update(uint8_t* h, int PT) {
    comp = (comp << 1) ^ h[PT & (HISTBUFFERLENGTH - 1)];
    comp ^= h[(PT + OLENGTH) & (HISTBUFFERLENGTH - 1)] << OUTPOINT;
    comp ^= (comp >> CLENGTH);
    comp = (comp) & ((1 << CLENGTH) - 1);
  }

  bool operator==(const cbp64_folded_history& other) const {
    return (comp == other.comp) && (CLENGTH == other.CLENGTH) && (OLENGTH == other.OLENGTH) &&
           (OUTPOINT == other.OUTPOINT);
  }
};

class cbp64_bentry  // TAGE bimodal table entry
{
 public:
  int8_t hyst;
  int8_t pred;

  cbp64_bentry() {
    pred = 0;

    hyst = 1;
  }

  bool operator==(const cbp64_bentry& other) const { return (pred == other.pred) && (hyst == other.hyst); }
};
class cbp64_gentry  // TAGE global table entry
{
 public:
  int8_t ctr;
  uint tag;
  int8_t u;

  cbp64_gentry() {
    ctr = 0;
    u = 0;
    tag = 0;
  }

  bool operator==(const cbp64_gentry& other) const {
    return (ctr == other.ctr) && (tag == other.tag) && (u == other.u);
  }
};

#define LOGL 5
#define WIDTHNBITERLOOP 10  // we predict only loops with less than 1K iterations
#define LOOPTAG 10          // tag width in the loop predictor

class cbp64_lentry  // loop predictor entry
{
 public:
  uint16_t NbIter;       // 10 bits
  uint8_t confid;        // 4bits
  uint16_t CurrentIter;  // 10 bits

  uint16_t TAG;  // 10 bits
  uint8_t age;   // 4 bits
  bool dir;      // 1 bit

  // 39 bits per entry
  cbp64_lentry() {
    confid = 0;
    CurrentIter = 0;
    NbIter = 0;
    TAG = 0;
    age = 0;
    dir = false;
  }

  bool operator==(const cbp64_lentry& other) const {
    return (NbIter == other.NbIter) && (confid == other.confid) && (CurrentIter == other.CurrentIter) &&
           (TAG == other.TAG) && (age == other.age) && (dir == other.dir);
  }
};

enum tage_component {
  TAGE_BASE,   // final prediction by GetPrediction() is bimodal table
  TAGE_SHORT,  // final prediction by GetPrediction() is TAGE component which are accessed by shorter history
               // lengths(MINHIST:6 ~ 37)
  TAGE_LONG,  // final prediction by GetPrediction() is TAGE component which are accessed by longer history lengths(54 ~
              // MAXHIST:3000)
  TAGE_LOOP,  // final prediction by GetPrediction() is LOOP predictor
  TAGE_SC,    // final prediction by GetPrediction() is statistical corrector(SC)
  NOT_TAGE    // redundancy
};

// due to partial associativity, twice the number of different histories (18 different histories)
#define HIST 18            // 18 different history lengths
#define ASSOC 2            // 2-way associativity
#define NHIST HIST* ASSOC  // able to support HIST * ASSOC histories
#define MANUAL_OMIT 4      // marginally remove some history lengths
// we use 2-way associativity for 5'th to 12'th history lengths out of 18
#define BORNINFASSOC (4 * ASSOC + 1)   // disable associativity for 1st 4 history lengths
#define BORNSUPASSOC (11 * ASSOC + 1)  // enable associativity for upto 11th history lengths
// the actual number of history lengths which are used in CBP64K
#define NOSKIPCNT                                                                                            \
  (NHIST - ((BORNINFASSOC - 1) * (ASSOC - 1) / ASSOC) - ((NHIST - BORNSUPASSOC + 1) * (ASSOC - 1) / ASSOC) - \
   MANUAL_OMIT)

#define LOGGNB 10  // 1 1K + 2 * 512-entry tables
#define GNB 3
#define PNB 3
#define LOGPNB 9  // 1 1K + 2 * 512-entry tables
#define RANDOMCNT 6
#define VARTHRES
#ifdef VARTHRES
#define LOGSIZEUP 6  // not worth increasing
#else
#define LOGSIZEUP 0
#endif
#define LOGSIZEUPS (LOGSIZEUP / 2)

// P State: Predictor (P) state is generated by TAGE at predict but can only be applied after the branch has been
// resolved successfully. Because there can be multiple unresolved branches in flight at the same time,
// we store the P state of every branch until we can perform the update (exec or commit).
// P state is used to update the N components.
struct PredictorStates {
  // Predictor status
  // SC
  int THRES;  // used for comparing SC and intermediate result(one of TAGE or LOOP) to determine final prediction
  int LSUM;   // SC predict using LSUM
  // LOOP
  bool predloop;  // prediction of LOOP predictor
  int LHIT;       // hitting way in LOOP predictor
  bool LVALID;    // validity of LOOP predictor prediction
  // TAGE
  bool pred_taken;        // final prediction
  bool tage_pred;         // prediction of TAGE: one of LongestMatchPred or alttaken
  bool pred_inter;        // intermediate result: one of TAGE or LOOP
  bool LongestMatchPred;  // prediction of selected bank which has the longest history length among hit banks in TAGE
  bool HighConf;          // Set high confidence when 2*|ctr + 1| >= 7
  bool MedConf;           // Set med confidence when 2*|ctr + 1| == 5
  bool LowConf;           // Set low confidence when 2*|ctr + 1| == 1
  int HitBank;            // index of the bank with the longest history among matching tags in TAGE table
  // ALT
  bool alttaken;          // prediction of alternative predictor
  bool AltConf;           // Set med confidence when 2*|ctr + 1| > 1
  int AltBank;            // index of the bank with 2nd longest history among matching tags in TAGE table
  // We are using on-path history as a seed for MYRANDOM() to ensure deterministic behavior.
  // Using both on/off-path history info should not change entropy of MYRANDOM() substantially
  int on_path_ptghist;
  long long on_path_phist;

  // regular pointer incurs Segmentation fault + automatic memory management
  std::unique_ptr<int[]> GI;     // hashed index set for gtable
  std::unique_ptr<uint[]> GTAG;  // hashed tag set for gtable

  // Constructor
  PredictorStates(bool snapshot = false) {
    int size = snapshot ? NOSKIPCNT : (NHIST + 1);
    GI = std::make_unique<int[]>(size);
    GTAG = std::make_unique<uint[]>(size);
  }

  // Move constructor & assignment
  PredictorStates(PredictorStates&& other) noexcept = default;
  PredictorStates& operator=(PredictorStates&& other) noexcept = default;

  // Delete copy constructor and assignment
  PredictorStates(const PredictorStates&) = delete;
  PredictorStates& operator=(const PredictorStates&) = delete;

  void init() {
    THRES = 0;
    LSUM = 0;
    predloop = false;
    LHIT = 0;
    LVALID = 0;
    pred_taken = false;
    tage_pred = false;
    pred_inter = false;
    LongestMatchPred = false;
    HighConf = false;
    MedConf = false;
    LowConf = false;
    HitBank = 0;
    alttaken = false;
    AltConf = false;
    AltBank = 0;
    on_path_ptghist = 0;
    on_path_phist = 0;
  }

  std::string to_string() const {
    std::stringstream ss;
    ss << "PState{" << "THRES=" << THRES << " LSUM=" << LSUM << " pred_taken=" << pred_taken
       << " tage_pred=" << tage_pred << " pred_inter=" << pred_inter << " LongestMatchPred=" << LongestMatchPred
       << " alttaken=" << alttaken << " HighConf=" << HighConf << " MedConf=" << MedConf << " LowConf=" << LowConf
       << " AltConf=" << AltConf << " HitBank=" << HitBank << " AltBank=" << AltBank << "}";
    // Print GI
    ss << " GI=[";
    for (int i = 1; i <= NHIST; i++) {
      if (i > 0)
        ss << ",";
      ss << GI[i];
    }
    ss << "]";
    // Print GTAG
    ss << " GTAG=[";
    for (int i = 1; i <= NHIST; i++) {
      if (i > 0)
        ss << ",";
      ss << GTAG[i];
    }
    ss << "]";
    return ss.str();
  }
};

// S components: Speculative (S) components need to be updated immediately after each branch prediction
// to enable accurate prediction of the following branches. Because these updates are speculative,
// we need to checkpoint and restore on a resolved branch misprediction.
struct SpeculativeStatesBase {
  // checkpoint doesn't need to snapshot ghist because ptghist overwrite off-path updates
  //  and ghist's length(3000-bit, around 1200 branches) is long enough to avoid overlapping of correct histories
  // TAGE
  int ptghist;      // pointer of ghist(global history)
  long long phist;  // path history
  // folded global history using cyclic shift register to generate i'th TAGE index
  cbp64_folded_history ch_i[NHIST + 1];
  // folded global history using cyclic shift register to generate i'th TAGE tag
  cbp64_folded_history ch_t[2][NHIST + 1];
  // SC
  long long GHIST;             // global history which is specialized to conditional branch
  int8_t WG[1 << LOGSIZEUPS];  // GGEHL's weight table
  int8_t WP[1 << LOGSIZEUPS];  // PGEHL's weight table
  // LOOP
  cbp64_lentry ltable[1 << LOGL];  // entire loop table.

  void init() {
    ptghist = 0;
    phist = 0;
    memset(ch_i, 0, sizeof(ch_i));
    memset(ch_t, 0, sizeof(ch_t));
    GHIST = 0;
    memset(WG, 0, sizeof(WG));
    memset(WP, 0, sizeof(WP));
    memset(ltable, 0, sizeof(ltable));
  }

  std::string to_string() const {
    std::stringstream ss;
    ss << "SpecBase{" << "ptghist=" << ptghist << " GHIST=" << GHIST << " phist=" << phist;

    // Print folded histories
    ss << " ch_i=[";
    for (int i = 0; i <= NHIST; i++) {
      if (i > 0)
        ss << ",";
      ss << ch_i[i].comp;
    }
    ss << "]";

    ss << " ch_t=[";
    for (int i = 0; i <= NHIST; i++) {
      if (i > 0)
        ss << ",";
      ss << "[" << ch_t[0][i].comp << "," << ch_t[1][i].comp << "]";
    }
    ss << "]";

    // Print weight tables
    ss << " WG=[";
    for (int i = 0; i < (1 << LOGSIZEUPS); i++) {
      if (i > 0)
        ss << ",";
      ss << (int)WG[i];
    }
    ss << "]";

    ss << " WP=[";
    for (int i = 0; i < (1 << LOGSIZEUPS); i++) {
      if (i > 0)
        ss << ",";
      ss << (int)WP[i];
    }
    ss << "]";
    if (TAGESCL64KB_LOOP) {
      ss << " ltable=[...omitted...]";  // Can expand if needed
    }

    ss << "}";
    return ss.str();
  }
};

struct SpeculativeStates : public SpeculativeStatesBase {
  // Copy data from GGEHLA and PGEHLA to GGEHL and PGEHL to share GEHL functions
  int8_t* GGEHL[GNB];  // GEHL component which exploits 'GHIST'
  int8_t* PGEHL[PNB];  // GEHL component which exploits 'phist'
  // This is restored by ptghist so you don't need to snapshot it
  uint8_t ghist[HISTBUFFERLENGTH];  // S: 3000-bit global history buffer (circular buffer)

  void init() {
    SpeculativeStatesBase::init();
    memset(GGEHL, 0, sizeof(GGEHL));
    memset(PGEHL, 0, sizeof(PGEHL));
    memset(ghist, 0, sizeof(ghist));
  }
};

struct Checkpoint : public SpeculativeStatesBase {
  int8_t GGEHL[GNB][1 << LOGGNB];  // GEHL component which exploits 'GHIST'
  int8_t PGEHL[PNB][1 << LOGPNB];  // GEHL component which exploits 'phist'
};

struct PredictorEntry {
  Counter counter;
  PredictorStates state;

  // Constructor that takes ownership of state
  PredictorEntry(const Counter& c, PredictorStates&& s) : counter(c), state(std::move(s)) {}
};

struct CheckpointEntry {
  Counter counter;
  Checkpoint state;

  CheckpointEntry(const Counter& c, const Checkpoint& cp) : counter(c), state(cp) {}
};

typedef boost::multi_index_container<
    PredictorEntry,
    // Ordered by Counter as the key (unique index)
    boost::multi_index::indexed_by<boost::multi_index::ordered_unique<
                                       boost::multi_index::member<PredictorEntry, Counter, &PredictorEntry::counter>>,
                                   // Sequence to maintain insertion order
                                   boost::multi_index::sequenced<>>>
    PredictorContainer;

typedef boost::multi_index_container<
    CheckpointEntry,
    // Ordered by Counter as the key (unique index)
    boost::multi_index::indexed_by<boost::multi_index::ordered_unique<
                                       boost::multi_index::member<CheckpointEntry, Counter, &CheckpointEntry::counter>>,
                                   // Sequence to maintain insertion order
                                   boost::multi_index::sequenced<>>>
    CheckpointContainer;

class TAGE64K {
 private:
  // The statistical corrector components

#define PERCWIDTH 6  // Statistical corrector  counter width 5 -> 6 : 0.6 %
  // The three BIAS tables in the SC component
  // We play with the TAGE  confidence here, with the number of the hitting bank
#define LOGBIAS 8
#define INDBIAS(state)                                                                                         \
  (((((PC ^ (PC >> 2)) << 1) ^ (state.LowConf & (TAGESCL64KB_ALT ? (state.LongestMatchPred != state.alttaken)  \
                                                                 : (state.LongestMatchPred > state.HitBank)))) \
    << 1) +                                                                                                    \
   state.pred_inter) &                                                                                         \
      ((1 << LOGBIAS) - 1)
#define INDBIASSK(state) \
  (((((PC ^ (PC >> (LOGBIAS - 2))) << 1) ^ (state.HighConf)) << 1) + state.pred_inter) & ((1 << LOGBIAS) - 1)
#define INDBIASBANK(state)                                                                              \
  (state.pred_inter + (((state.HitBank + 1) / 4) << 4) + (state.HighConf << 1) + (state.LowConf << 2) + \
   ((TAGESCL64KB_ALT ? (state.AltBank != 0) : 0) << 3) + ((PC ^ (PC >> 2)) << 7)) &                     \
      ((1 << LOGBIAS) - 1)

  // IMLI-SIC -> Micro 2015  paper: a big disappointment on  CBP2016 traces
#ifdef IMLI
#define LOGINB 8  // 128-entry
#define INB 1
  int Im[INB] = {8};
  int8_t IGEHLA[INB][(1 << LOGINB)] = {{0}};

#define LOGIMNB 9  // 2* 256 -entry
#define IMNB 2
  int IMm[IMNB] = {10, 4};
  int8_t IMGEHLA[IMNB][(1 << LOGIMNB)] = {{0}};
#endif

  // global branch GEHL
#define LOGGNB 10  // 1 1K + 2 * 512-entry tables
#define GNB 3
  int Gm[GNB] = {40, 24, 10};
  int8_t GGEHLA[GNB][(1 << LOGGNB)] = {{0}};

  // variation on global branch history
#define PNB 3
#define LOGPNB 9  // 1 1K + 2 * 512-entry tables
  int Pm[PNB] = {25, 16, 9};
  int8_t PGEHLA[PNB][(1 << LOGPNB)] = {{0}};

  // first local history
#define LOGLNB 10  // 1 1K + 2 * 512-entry tables
#define LNB 3
  int Lm[LNB] = {11, 6, 3};
  int8_t LGEHLA[LNB][(1 << LOGLNB)] = {{0}};

#define LOGLOCAL 8
#define NLOCAL (1 << LOGLOCAL)
#define INDLOCAL ((PC ^ (PC >> 2)) & (NLOCAL - 1))

  // second local history
#define LOGSNB 9  // 1 1K + 2 * 512-entry tables
#define SNB 3
  int Sm[SNB] = {16, 11, 6};
  int8_t SGEHLA[SNB][(1 << LOGSNB)] = {{0}};

#define LOGSECLOCAL 4
#define NSECLOCAL (1 << LOGSECLOCAL)  // Number of second local histories
#define INDSLOCAL (((PC ^ (PC >> 5))) & (NSECLOCAL - 1))

  // third local history
#define LOGTNB 10  // 2 * 512-entry tables
#define TNB 2
  int Tm[TNB] = {9, 4};
  int8_t TGEHLA[TNB][(1 << LOGTNB)] = {{0}};

#define NTLOCAL 16
#define INDTLOCAL (((PC ^ (PC >> (LOGTNB)))) & (NTLOCAL - 1))  // different hash for the history

  // playing with putting more weights (x2)  on some of the SC components
  // playing on using different update thresholds on SC
  // update threshold for the statistical corrector
#define VARTHRES
#define WIDTHRES 12
#define WIDTHRESP 8
#ifdef VARTHRES
#define LOGSIZEUP 6  // not worth increasing
#else
#define LOGSIZEUP 0
#endif
#define LOGSIZEUPS (LOGSIZEUP / 2)
#define INDUPD (PC ^ (PC >> 2)) & ((1 << LOGSIZEUP) - 1)
#define INDUPDS ((PC ^ (PC >> 2)) & ((1 << (LOGSIZEUPS)) - 1))
#define EWIDTH 6
#define POWER
  // use geometric history length
#define NBANKLOW 10   // number of banks in the shared bank-interleaved for the low history lengths
#define NBANKHIGH 20  // number of banks in the shared bank-interleaved for the  history lengths
  int SizeTable[NHIST + 1];

#define BORN 13  // below BORN in the table for low history lengths, >= BORN in the table for high history lengths,

  /*in practice 2 bits or 3 bits par branch: around 1200 cond. branchs*/
#define MINHIST 6  // not optimized so far
#define MAXHIST 3000

#define LOGG 10            /* logsize of the  banks in the  tagged TAGE tables */
#define TBITS 8            // minimum width of the tags  (low history lengths), +4 for high history lengths
  bool NOSKIP[NHIST + 1];  // to manage the associativity for different history lengths

#define NNN 1        // number of extra entries allocated on a TAGE misprediction (1+NNN)
#define HYSTSHIFT 2  // bimodal hysteresis shared by 4 entries
#define LOGB 13      // log of number of entries in bimodal predictor

#define PHISTWIDTH 27  // width of the path history used in TAGE
#define UWIDTH 1       // u counter width on TAGE (2 bits not worth the effort for a 512 Kbits predictor 0.2 %)
#define CWIDTH 3       // predictor counter width on the TAGE tagged tables

// the counter(s) to chose between longest match and alternate prediction on TAGE when weak counters
#define LOGSIZEUSEALT 4
#define ALTWIDTH 5
#define SIZEUSEALT (1 << (LOGSIZEUSEALT))
#define INDUSEALT(state) (((((state.HitBank - 1) / 8) << 1) + state.AltConf) % (SIZEUSEALT - 1))

  int m[NHIST + 1];
  int TB[NHIST + 1];
  int logg[NHIST + 1];
  int8_t noskip_index[NHIST + 1];

 public:
  TAGE64K(void);
  uns8 IsFull(void);
  void reinit();
  int F(long long A, int size, int bank);
  int gindex(unsigned int PC, int bank, long long hist, cbp64_folded_history* ch_i);
  uint16_t gtag(unsigned int PC, int bank, cbp64_folded_history* ch0, cbp64_folded_history* ch1);
  void ctrupdate(int8_t& ctr, bool taken, int nbits);
  bool getbim(UINT64 PC);
  void baseupdate(bool Taken, UINT64 PC);
  int MYRANDOM(long long on_path_phist, int on_path_ptghist, bool off_path);
  void Tagepred(UINT64 PC);
  void UpdateAddr(UINT64 PC, long long path_history, cbp64_folded_history* index, cbp64_folded_history* tag0,
                  cbp64_folded_history* tag1);
  bool GetPrediction(UINT64 PC, int* bp_confidence, Op* op);
  void HistoryUpdate(UINT64 PC, OpType opType, bool taken, UINT64 target);
  void SavePredictorStates(Counter key);
  void TakeCheckpoint(Counter key);
  void RetireCheckpoint(Counter key);
  void VerifyCheckpoint(Counter key);
  void VerifyPredictorStates(Counter key);
  void RestoreStates(Counter key, UINT64 PC, OpType optype, Flag is_conditional, Flag dir, UINT64 target);
  void RestoreCheckpoint(Counter key);
  void RestorePredictorstates(Counter key);
  void ComparePredictor(const PredictorStates& Pstate);
  void CompareCheckpoint(const Checkpoint& cp);
  Counter KeyGeneration();
  int GetBrtypeFromOptype(OpType opType);
  void UpdatePredictor(UINT64 PC, OpType opType, bool resolveDir, bool predDir, UINT64 branchTarget,
                       const PredictorStates& Pstate);
  void SpecUpdate(UINT64 PC, OpType opType, bool predDir, UINT64 branchTarget);
  void GlobalStateUpdate(UINT64 PC, UINT64 branchTarget, int brtype, bool predDir);
  void SpecUpdateAtCond(UINT64 PC, bool predDir, bool off_path);
  void NonSpecUpdateAtCond(UINT64 PC, OpType opType, bool resolveDir, bool predDir, UINT64 branchTarget,
                           Counter branch_id);
  int Gpredict(UINT64 PC, long long BHIST, int* length, int8_t** tab, int NBR, int logs, int8_t* W);
  void Gupdate(UINT64 PC, bool taken, long long BHIST, int* length, int8_t** tab, int NBR, int logs, int8_t* W,
               int LSUM);
  void TrackOtherInst(UINT64 PC, OpType opType, bool taken, UINT64 branchTarget);
  int lindex(UINT64 PC);
  bool getloop(UINT64 PC);
  void SpecLoopUpdate(UINT64 PC, bool Taken, long long on_path_phist, int on_path_ptghist, bool off_path);
  void LoopUpdate(UINT64 PC, bool Taken, bool ALLOC, int lhit, long long on_path_phist, int on_path_ptghist);

  // P: Predictor components
  PredictorStates Pstate;
  // S: Speculative components
  SpeculativeStates Sstate;
  // N components are not speculative. They are only updated after a branch is successfully resolved.
  // We utilize per branch P-state to update N components.
  int8_t Bias[(1 << LOGBIAS)];      // N: BIAS table for SC
  int8_t BiasSK[(1 << LOGBIAS)];    // N: BIAS table for SKIP
  int8_t BiasBank[(1 << LOGBIAS)];  // N: BIAS table for BANK
  int8_t* LGEHL[LNB];               // N: GEHL for first local history
  int8_t* SGEHL[SNB];               // N: GEHL for second local history
  int8_t* TGEHL[TNB];               // N: GEHL for third local history
#ifdef IMLI
  int8_t* IMGEHL[IMNB];   // N: GEHL for IMLI
  int8_t* IGEHL[INB];     // N: GEHL for IMLI
  long long IMHIST[256];  // N: IMHIST
  long long IMLIcount;    // N: use to monitor the iteration number
#endif
  long long L_shist[NLOCAL];      // N: local histories
  long long S_slhist[NSECLOCAL];  // N: second local history
  long long T_slhist[NTLOCAL];    // N: third local history
  // playing with putting more weights (x2)  on some of the SC components
  int updatethreshold;                     // N: update threshold for the statistical corrector
  int Pupdatethreshold[(1 << LOGSIZEUP)];  // N: size is fixed by LOGSIZEUP
  int8_t WL[(1 << LOGSIZEUPS)];            // N: GEHL weights for local history
  int8_t WS[(1 << LOGSIZEUPS)];            // N: GEHL weights for second local history
  int8_t WT[(1 << LOGSIZEUPS)];            // N: GEHL weights for third local history
  int8_t WI[(1 << LOGSIZEUPS)];            // N: GEHL weights for IMLI
  int8_t WIM[(1 << LOGSIZEUPS)];           // N: GEHL weights for IMHIST
  int8_t WB[(1 << LOGSIZEUPS)];            // N: GEHL weights for Bias
  int8_t FirstH, SecondH;                  // N: counters to choose between TAGE and SC on Low Conf SC
  // LOOP
  int8_t WITHLOOP;  // N: counter to monitor whether or not loop prediction is beneficial
  int LIB;
  int LI;
  int LTAG;  // tag on the loop predictor
  // ALT
  int8_t use_alt_on_na[SIZEUSEALT];  // N: counters to choose between longest match and second longest match on TAGE
  // TAGE
  cbp64_bentry* btable;              // N: bimodal TAGE table
  cbp64_gentry* gtable[NHIST + 1];   // N: tagged TAGE tables

  // utility variables
  int TICK;  // N: for the reset of the u counter
  Counter branch_id;
  int Seed;       // for the pseudo-random number generator
  // snapshot containers
  CheckpointContainer checkpoints;
  PredictorContainer predictor_states;

  int8_t tage_component;
  int8_t tage_component_inter;
  int8_t tage_component_tage;
  int8_t tage_component_alt;
};

#endif
