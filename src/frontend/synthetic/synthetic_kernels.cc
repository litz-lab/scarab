#include "frontend/synthetic/synthetic_kernels.h"

#include <algorithm>
#include <iostream>
#include <random>

#include "bp/bp.param.h"
#include "memory/memory.param.h"

#define NOP_SIZE ICACHE_LINE_SIZE / (ISSUE_WIDTH)
#define BRANCH_SIZE ICACHE_LINE_SIZE - (NOP_SIZE * (ISSUE_WIDTH - 1))
#define LOAD_SIZE 8
#define ADD_SIZE 8
#define WORKLOAD_LENGTH 1000

/* Bottleneck name strings */
const char* bottleneckNames[] = {
#define BOTTLENECK_IMPL(id, name) name,
#include "bottlenecks_table.def"
#undef BOTTLENECK_IMPL
    "invalid"};

BottleNeck_enum bottleneck;
uns64 synth_start_pc{256};
uns64 synth_start_uid{1000};
/*Random Geneatation Utilities*/
static std::mt19937_64 engine(1234);
static std::uniform_int_distribution<uns64> dist64{1, 0x00007fffffffffff};
static std::bernoulli_distribution distBool(0.5);
/* Helper vars */
uns cf_count = 0;
static const uns64 start_ld_vaddr{256};
static uns64 ld_vaddr{start_ld_vaddr};
static std::vector<uns64> vaddr;
static uns64 tgtAddr{0};
static bool direction{false};
static std::vector<uns64> branch_targets;
static uns64 index_count{0};
static bool loopback_ibr[MAX_NUM_PROCS];
static uns64 accumulated_workload_size{0};
static const uns64 loopback_ip{1256};
/********************************************** Utilities **************************************************** */
void gen_vaddr();
void gen_branch_targets();

/* The BR workloads are tricky and can induce frontend bandwidth stalls even where every issue paccket is a    factor
of the cache line size, the following function restricts both onpath and offpath issue packets to the length   of  an
icache line by preceeding branches with enough NOPs so that the issue packet equals ICACHE_LINE_SIZE. For a not-taken
branch, the next address starts at the immediately following cache line, for a taken branch the next  address      is
two cache lines away */
template <typename branch_gen>
ctype_pin_inst lock_issue_packet_to_icache_boundary(uns64, uns64, branch_gen);

/********************************************** Kernel Dispatcher ******************************************** */
ctype_pin_inst generate_synthetic_microkernel(uns proc_id, BottleNeck_enum bottleneck_type, uns64 ip, uns64 uid,
                                              bool offpath) {
  switch (bottleneck_type) {
    case MEM_BANDWIDTH_LIMITED:
      return generate_mem_bandwidth_limited_microkernel(ip, uid, offpath);
    case MEM_LATENCY_LIMITED:
      return generate_mem_latency_limited_microkernel(ip, uid, offpath);
    case CBR_LIMITED:
      return generate_cbr_limited_microkernel(ip, uid, offpath);
    case BTB_LIMITED:
      return generate_btb_limited_microkernel(ip, uid, offpath);
    case ICACHE_LIMITED:
      return generate_icache_limited_microkernel(ip, uid, offpath);
    case IBR_LIMITED:
      return generate_ibr_limited_microkernel(ip, uid, proc_id, offpath);
    case ILP_LIMITED:
      return create_ILP_limited_microkernel(ip, uid, offpath);
    case BTB_ASSOCIATIVITY_LIMITED:
      return generate_btb_assoc_limited_microkernel(ip, uid, offpath);
    default:
      return create_dummy_nop(ip, WPNM_NOT_IN_WPNM);
  }
}

/********************************************** Kernels ******************************************************* */
/*
  The control flow workloads break after the most recent commit, consecutivity assertion in ft.cc fails when the
  execution goes offpath. This happens when a branch(usually situated at an ICACHE_BOUNDARY) is encountered on the
  offpath. A workaround so that the workloads can run was to generate only NOPs until recovery. Once the intial issue is
  fixed, the CBR kernels should revert to generating the pattern as seen on the onpath.
*/
ctype_pin_inst generate_cbr_limited_microkernel(uns64 ip, uns64 uid, bool offpath) {
  ctype_pin_inst inst;
  if (!offpath) {
    inst = lock_issue_packet_to_icache_boundary(ip, uid, [&]() -> ctype_pin_inst {
      if (ip >= loopback_ip) {
        tgtAddr = synth_start_pc;
        return generate_unconditional_branch(ip, synth_start_uid, synth_start_pc, BRANCH_SIZE);
      } else {
        tgtAddr = ip + ICACHE_LINE_SIZE + BRANCH_SIZE;
        return generate_conditional_branch(ip, uid, tgtAddr, direction, BRANCH_SIZE);
      }
    });
    direction = distBool(engine);
  } else
    return make_nop(ip, uid, NOP_SIZE, false);
  return inst;
}

ctype_pin_inst generate_btb_limited_microkernel(uns64 ip, uns64 uid, bool offpath) {
  ctype_pin_inst inst;
  if (!offpath) {
    inst = lock_issue_packet_to_icache_boundary(ip, uid, [&]() -> ctype_pin_inst {
      if (index_count == BTB_ENTRIES + 1) {
        index_count = 0;
        std::shuffle(branch_targets.begin(), branch_targets.end(), engine);
        return generate_unconditional_branch(ip, synth_start_uid, synth_start_pc, BRANCH_SIZE);
      } else {
        return generate_unconditional_branch(ip, uid, branch_targets[index_count++], BRANCH_SIZE);
      }
    });
  } else
    return make_nop(ip, uid, NOP_SIZE, false);
  return inst;
}

ctype_pin_inst generate_ibr_limited_microkernel(uns64 ip, uns64 uid, uns64 proc_id, bool offpath) {
  ctype_pin_inst inst;
  if (!offpath) {
    inst = lock_issue_packet_to_icache_boundary(ip, uid, [&]() -> ctype_pin_inst {
      if (loopback_ibr[proc_id]) {
        loopback_ibr[proc_id] = false;
        return generate_unconditional_branch(ip, synth_start_uid, synth_start_pc, BRANCH_SIZE);
      } else {
        loopback_ibr[proc_id] = true;
        return generate_indirect_branch(ip, uid, branch_targets[distBool(engine)], vaddr[index_count], BRANCH_SIZE);
      }
    });
  } else
    return make_nop(ip, uid, NOP_SIZE, false);
  return inst;
}

ctype_pin_inst generate_mem_latency_limited_microkernel(uns64 ip, uns64 uid, bool offpath) {
  ctype_pin_inst inst;
  if (!offpath && ip >= loopback_ip) {
    inst = generate_unconditional_branch(ip, synth_start_uid, synth_start_pc, BRANCH_SIZE);
    gen_vaddr();
    index_count = 0;
  } else {
    inst = generate_loop_carried_dependence_load(ip, uid, vaddr[index_count], LOAD_SIZE);
    index_count++;
  }
  return inst;
}

ctype_pin_inst generate_mem_bandwidth_limited_microkernel(uns64 ip, uns64 uid, bool offpath) {
  ctype_pin_inst inst;
  if (!offpath && ip >= loopback_ip) {
    inst = generate_unconditional_branch(ip, synth_start_uid, synth_start_pc, BRANCH_SIZE);
    ld_vaddr = start_ld_vaddr;
  } else {
    inst = generate_independent_operand_load(ip, uid, ld_vaddr, LOAD_SIZE);
    // ensure we are always in user space
    ld_vaddr = (ld_vaddr + 8) % 0x0000800000000000;
  }
  return inst;
}

ctype_pin_inst create_ILP_limited_microkernel(uns64 ip, uns64 uid, bool offpath) {
  ctype_pin_inst inst;
  if (!offpath && ip >= loopback_ip) {
    inst = generate_unconditional_branch(ip, synth_start_uid, synth_start_pc, BRANCH_SIZE);
  } else
    inst = generate_alu_type_inst(ip, uid, ADD_SIZE);
  return inst;
}

ctype_pin_inst generate_icache_limited_microkernel(uns64 ip, uns64 uid, bool offpath) {
  if (!offpath) {
    if (accumulated_workload_size >= 2 * ICACHE_SIZE) {
      accumulated_workload_size = 0;
      return generate_unconditional_branch(ip, synth_start_uid, synth_start_pc, 64);
    }
    accumulated_workload_size += 64;
  }
  return generate_no_dependence_alu_type_inst(ip, uid, 64);
}

ctype_pin_inst generate_btb_assoc_limited_microkernel(uns64 ip, uns64 uid, bool offpath) {
  ctype_pin_inst inst;
  if (!offpath) {
    inst = lock_issue_packet_to_icache_boundary(ip, uid, [&]() -> ctype_pin_inst {
      if (index_count == 2 * BTB_ASSOC) {
        index_count = 0;
        return generate_unconditional_branch(ip, synth_start_uid, synth_start_pc, BRANCH_SIZE);
      } else {
        return generate_unconditional_branch(ip, uid, branch_targets[index_count++], BRANCH_SIZE);
      }
    });
  } else
    return make_nop(ip, uid, NOP_SIZE, false);
  return inst;
}

/************************************** Utility Definitions ************************************ */
void gen_branch_targets() {
  uns64 i{synth_start_pc};
  for (uns64 j{0}; j < (BTB_ENTRIES + 2048); j++) {
    i = bottleneck == BTB_ASSOCIATIVITY_LIMITED ? (i + 2048) : (i + 128);
    branch_targets.push_back(i);
  }
}

void synthetic_kernel_init() {
  std::cout << "Simulating synthetic " << bottleneckNames[BOTTLENECK] << " bottleneck" << std::endl;
#ifdef PRINT_CF_NOP_SIZE_INFO
  std::cout << " NOP SIZE " << NOP_SIZE << " BRANCH SIZE " << BRANCH_SIZE << std::endl;
  std::cout << "ICACHE_SIZE " << ICACHE_SIZE << std::endl;
#endif
  gen_branch_targets();
  gen_vaddr();
}

void gen_vaddr() {
  vaddr.clear();
  for (int i{0}; i < 1000; i++)
    vaddr.push_back(dist64(engine));
}

template <typename branch_gen>  // function template
ctype_pin_inst lock_issue_packet_to_icache_boundary(uns64 ip, uns64 uid, branch_gen generate_branch) {
  if (cf_count == ISSUE_WIDTH - 1) {
    cf_count = 0;
    return generate_branch();
  } else {
    cf_count++;
    return make_nop(ip, uid, NOP_SIZE, false);
  }
}
