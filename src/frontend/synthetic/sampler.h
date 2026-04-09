#ifndef SAMPLER_H
#define SAMPLER_H

#include <globals/global_types.h>
#include <random>
#include <vector>

enum Sequence_Pick_Strategy {
  UNIFORM_SEQUENTIAL,  // pick sequentially
  UNIFORM_RANDOM,      // pick sequentially but sequence vector must be shuffled
  NORMAL_RANDOM,       // use the formula
  USER_DEFINED         // user supplied sequence
};

class Sampler {
  std::vector<uns64> sequence_vector;
  Sequence_Pick_Strategy strategy;
  uns64 next_pick_index;
  void scale_periodicity(uns64 periodicity);

 public:
  // generates the sequence accorfing to stride, periodicity and/or shuffles based on strategy
  Sampler(Sequence_Pick_Strategy strategy, uns64 value_range, uns64 startval, uns64 stride, uns periodicity = 1,
          std::vector<uns64> user_seq = {});

  /* user supplied sequence that is either shuffled, duplicated based on stride or used to create a discrete
    distribution based on weights */
  Sampler(std::vector<uns64> discrete_values, Sequence_Pick_Strategy strategy, uns64 periodicity, uns sequence_length,
          std::vector<uns64> discrete_weights);

  // get next element
  uns64 get_next_element();

  // peek element 2 indicies away. Useful for CF workloads, does not progress the element pointer.
  uns64 peek_element_following_next() const;

  // CF workloads need to know what the last target is, to keep workload bounded
  uns64 get_last_element() const { return sequence_vector.back(); }

  // RANDOM NUMBER GENERATOR
  static std::default_random_engine rng_engine;
};

#endif