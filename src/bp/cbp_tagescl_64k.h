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
#include <array>
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
  unsigned comp = 0;
  int CLENGTH = 0;
  int OLENGTH = 0;
  int OUTPOINT = 0;

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
  int THRES = 0;  // used for comparing SC and intermediate result(one of TAGE or LOOP) to determine final prediction
  int LSUM = 0;   // SC predict using LSUM
  // LOOP
  bool predloop = false;  // prediction of LOOP predictor
  int LHIT = 0;           // hitting way in LOOP predictor
  bool LVALID = false;    // validity of LOOP predictor prediction
  // TAGE
  bool pred_taken = false;  // final prediction
  bool tage_pred = false;   // prediction of TAGE: one of LongestMatchPred or alttaken
  bool pred_inter = false;  // intermediate result: one of TAGE or LOOP
  bool LongestMatchPred =
      false;              // prediction of selected bank which has the longest history length among hit banks in TAGE
  bool HighConf = false;  // Set high confidence when 2*|ctr + 1| >= 7
  bool MedConf = false;   // Set med confidence when 2*|ctr + 1| == 5
  bool LowConf = false;   // Set low confidence when 2*|ctr + 1| == 1
  int HitBank = 0;        // index of the bank with the longest history among matching tags in TAGE table
  // ALT
  bool alttaken = false;  // prediction of alternative predictor
  bool AltConf = false;   // Set med confidence when 2*|ctr + 1| > 1
  int AltBank = 0;        // index of the bank with 2nd longest history among matching tags in TAGE table
  // We are using on-path history as a seed for MYRANDOM() to ensure deterministic behavior.
  // Using both on/off-path history info should not change entropy of MYRANDOM() substantially
  int on_path_ptghist = 0;
  long long on_path_phist = 0;

  // Hashed index/tag sets for gtable. std::vector owns its storage and value-
  // initializes to zero, so no explicit new/delete or make_unique is needed.
  std::vector<int> GI = {};     // hashed index set for gtable
  std::vector<uint> GTAG = {};  // hashed tag set for gtable

  PredictorStates(bool snapshot = false) {
    init();
    int size = snapshot ? NOSKIPCNT : (NHIST + 1);
    GI.assign(size, 0);
    GTAG.assign(size, 0);
  }

  PredictorStates(PredictorStates&& other) noexcept = default;
  PredictorStates& operator=(PredictorStates&& other) noexcept = default;

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
  int ptghist = 0;      // pointer of ghist(global history)
  long long phist = 0;  // path history
  // folded global history using cyclic shift register to generate i'th TAGE index
  std::array<cbp64_folded_history, NHIST + 1> ch_i = {};
  // folded global history using cyclic shift register to generate i'th TAGE tag
  std::array<std::array<cbp64_folded_history, NHIST + 1>, 2> ch_t = {};
  // SC
  long long GHIST = 0;                          // global history which is specialized to conditional branch
  std::array<int8_t, 1 << LOGSIZEUPS> WG = {};  // GGEHL's weight table
  std::array<int8_t, 1 << LOGSIZEUPS> WP = {};  // PGEHL's weight table
  // LOOP
  std::array<cbp64_lentry, 1 << LOGL> ltable = {};  // entire loop table.

  void init() {
    ptghist = 0;
    phist = 0;
    ch_i = {};
    ch_t = {};
    GHIST = 0;
    WG = {};
    WP = {};
    ltable = {};
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
  std::array<int8_t*, GNB> GGEHL = {};  // GEHL component which exploits 'GHIST'
  std::array<int8_t*, PNB> PGEHL = {};  // GEHL component which exploits 'phist'
  // This is restored by ptghist so you don't need to snapshot it
  std::array<uint8_t, HISTBUFFERLENGTH> ghist = {};  // S: 3000-bit global history buffer (circular buffer)

  void init() {
    SpeculativeStatesBase::init();
    GGEHL = {};
    PGEHL = {};
    ghist = {};
  }
};

struct Checkpoint : public SpeculativeStatesBase {
  std::array<std::array<int8_t, 1 << LOGGNB>, GNB> GGEHL = {};  // GEHL component which exploits 'GHIST'
  std::array<std::array<int8_t, 1 << LOGPNB>, PNB> PGEHL = {};  // GEHL component which exploits 'phist'
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
 protected:
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
  std::array<int, INB> Im = {8};
  std::array<std::array<int8_t, 1 << LOGINB>, INB> IGEHLA = {};

#define LOGIMNB 9  // 2* 256 -entry
#define IMNB 2
  std::array<int, IMNB> IMm = {10, 4};
  std::array<std::array<int8_t, 1 << LOGIMNB>, IMNB> IMGEHLA = {};
#endif

  // global branch GEHL
#define LOGGNB 10  // 1 1K + 2 * 512-entry tables
#define GNB 3
  std::array<int, GNB> Gm = {40, 24, 10};
  std::array<std::array<int8_t, 1 << LOGGNB>, GNB> GGEHLA = {};

  // variation on global branch history
#define PNB 3
#define LOGPNB 9  // 1 1K + 2 * 512-entry tables
  std::array<int, PNB> Pm = {25, 16, 9};
  std::array<std::array<int8_t, 1 << LOGPNB>, PNB> PGEHLA = {};

  // first local history
#define LOGLNB 10  // 1 1K + 2 * 512-entry tables
#define LNB 3
  std::array<int, LNB> Lm = {11, 6, 3};
  std::array<std::array<int8_t, 1 << LOGLNB>, LNB> LGEHLA = {};

#define LOGLOCAL 8
#define NLOCAL (1 << LOGLOCAL)
#define INDLOCAL ((PC ^ (PC >> 2)) & (NLOCAL - 1))

  // second local history
#define LOGSNB 9  // 1 1K + 2 * 512-entry tables
#define SNB 3
  std::array<int, SNB> Sm = {16, 11, 6};
  std::array<std::array<int8_t, 1 << LOGSNB>, SNB> SGEHLA = {};

#define LOGSECLOCAL 4
#define NSECLOCAL (1 << LOGSECLOCAL)  // Number of second local histories
#define INDSLOCAL (((PC ^ (PC >> 5))) & (NSECLOCAL - 1))

  // third local history
#define LOGTNB 10  // 2 * 512-entry tables
#define TNB 2
  std::array<int, TNB> Tm = {9, 4};
  std::array<std::array<int8_t, 1 << LOGTNB>, TNB> TGEHLA = {};

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
  std::array<int, NHIST + 1> SizeTable = {};

#define BORN 13  // below BORN in the table for low history lengths, >= BORN in the table for high history lengths,

  /*in practice 2 bits or 3 bits par branch: around 1200 cond. branchs*/
#define MINHIST 6  // not optimized so far
#define MAXHIST 3000

#define LOGG 10            /* logsize of the  banks in the  tagged TAGE tables */
#define TBITS 8            // minimum width of the tags  (low history lengths), +4 for high history lengths
  std::array<bool, NHIST + 1> NOSKIP = {};  // to manage the associativity for different history lengths

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

  std::array<int, NHIST + 1> m = {};
  std::array<int, NHIST + 1> TB = {};
  std::array<int, NHIST + 1> logg = {};
  std::array<int8_t, NHIST + 1> noskip_index = {};

 public:
  TAGE64K(void);
  ~TAGE64K();
  TAGE64K& operator=(const TAGE64K& other);  // Copy assignment
  TAGE64K(TAGE64K&&) = delete;
  TAGE64K(const TAGE64K&) = delete;
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
  std::array<int8_t, 1 << LOGBIAS> Bias = {};      // N: BIAS table for SC
  std::array<int8_t, 1 << LOGBIAS> BiasSK = {};    // N: BIAS table for SKIP
  std::array<int8_t, 1 << LOGBIAS> BiasBank = {};  // N: BIAS table for BANK
  std::array<int8_t*, LNB> LGEHL = {};             // N: GEHL for first local history
  std::array<int8_t*, SNB> SGEHL = {};             // N: GEHL for second local history
  std::array<int8_t*, TNB> TGEHL = {};             // N: GEHL for third local history
#ifdef IMLI
  std::array<int8_t*, IMNB> IMGEHL = {};   // N: GEHL for IMLI
  std::array<int8_t*, INB> IGEHL = {};     // N: GEHL for IMLI
  std::array<long long, 256> IMHIST = {};  // N: IMHIST
  long long IMLIcount = 0;                 // N: use to monitor the iteration number
#endif
  std::array<long long, NLOCAL> L_shist = {};      // N: local histories
  std::array<long long, NSECLOCAL> S_slhist = {};  // N: second local history
  std::array<long long, NTLOCAL> T_slhist = {};    // N: third local history
  // playing with putting more weights (x2)  on some of the SC components
  int updatethreshold = 0;                                // N: update threshold for the statistical corrector
  std::array<int, 1 << LOGSIZEUP> Pupdatethreshold = {};  // N: size is fixed by LOGSIZEUP
  std::array<int8_t, 1 << LOGSIZEUPS> WL = {};            // N: GEHL weights for local history
  std::array<int8_t, 1 << LOGSIZEUPS> WS = {};            // N: GEHL weights for second local history
  std::array<int8_t, 1 << LOGSIZEUPS> WT = {};            // N: GEHL weights for third local history
  std::array<int8_t, 1 << LOGSIZEUPS> WI = {};            // N: GEHL weights for IMLI
  std::array<int8_t, 1 << LOGSIZEUPS> WIM = {};           // N: GEHL weights for IMHIST
  std::array<int8_t, 1 << LOGSIZEUPS> WB = {};            // N: GEHL weights for Bias
  int8_t FirstH = 0;
  int8_t SecondH = 0;
  // LOOP
  int8_t WITHLOOP = 0;  // N: counter to monitor whether or not loop prediction is beneficial
  int LIB = 0;
  int LI = 0;
  int LTAG = 0;  // tag on the loop predictor
  // ALT
  std::array<int8_t, SIZEUSEALT> use_alt_on_na =
      {};  // N: counters to choose between longest match and second longest match on TAGE
  // TAGE. btable/gtable_{low,high} own their storage via std::vector so that
  // construction/destruction and deep copy are handled without explicit
  // new/delete. gtable[] holds non-owning row pointers into that storage.
  std::vector<cbp64_bentry> btable = {};       // N: bimodal TAGE table storage
  std::vector<cbp64_gentry> gtable_low = {};   // N: backing storage for short-history TAGE tables
  std::vector<cbp64_gentry> gtable_high = {};  // N: backing storage for long-history TAGE tables
  cbp64_gentry* gtable[NHIST + 1] = {};        // N: tagged TAGE tables (non-owning row pointers)

  // utility variables
  int TICK = 0;  // N: for the reset of the u counter
  Counter branch_id = 0;
  int Seed = 0;  // for the pseudo-random number generator
  // snapshot containers
  CheckpointContainer checkpoints = {};
  PredictorContainer predictor_states = {};

  int8_t tage_component = 0;
  int8_t tage_component_inter = 0;
  int8_t tage_component_tage = 0;
  int8_t tage_component_alt = 0;
};
#endif
