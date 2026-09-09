#include <algorithm>
#include <globals/global_types.h>
#include <iostream>
#include <random>
#include <vector>

#include "sampler_params.h"

#ifndef SAMPLER_H
#define SAMPLER_H

class Sampler {
  std::vector<uns64> sequence_vector;
  Sequence_Pick_Strategy strategy;
  uns64 next_pick_index;
  uns64 last_element_in_sorted_order;
  void scale_periodicity(uns64 periodicity);

  // random number generator
  std::default_random_engine rng_engine;

  /********************** FIXES ******************/
  // generates a linear sequence vector
  std::vector<uns64> generate_range_based_sequence(uns64 value_range, uns64 startval, uns64 stride);
  // generates a sequence vector based on normal curve (gaussian)
  std::vector<uns64> generate_normal_curve_sequence(uns64 value_range, uns64 startval, uns64 stride);
  // generates a sequence vector based on user provided sequence and weights
  std::vector<uns64> generate_discrete_weight_sequence(std::vector<uns64> values, uns sequence_length,
                                                       std::vector<uns64> weights = {});

 public:
  void reset_next_pointer() { next_pick_index = 0; }

  void randomize_sequence_vector() { std::shuffle(sequence_vector.begin(), sequence_vector.end(), rng_engine); }
  // get next element
  uns64 get_next_element();

  // peek at next element, does not progress pointer. Useful for workloads where targets are picked from a random pool.
  uns64 peek_next_element() const;

  // peek element 2 indicies away. Useful for CF workloads, does not progress the element pointer.
  uns64 peek_element_following_next() const;

  // CF workloads need to know what the last target is, to keep workload bounded
  uns64 get_last_element() const { return sequence_vector.back(); }

  // CF workloads need to know what the last target is, to keep workload bounded
  uns64 get_last_element_in_sorted_order() const { return last_element_in_sorted_order; }

  // Common Constructor
  Sampler(std::vector<uns64> sequence_vector, Sequence_Pick_Strategy strategy, uns periodicity, bool should_shuffle);

  // Builds UNIFORM_SEQUENTIAL, NORMAL_RANDOM or UNIFORM_RANDOM sequence and passes control to common constructor
  Sampler(Sequence_Pick_Strategy strategy, uns periodicity, uns64 value_range, uns64 startval, uns64 stride);

  // Builds DISCRETE_RANDOM or retains USER_DEFINED sequence vector and passes control to common constructor
  Sampler(Sequence_Pick_Strategy strategy, uns periodicity, std::vector<uns64> values, uns sequence_length,
          std::vector<uns64> weights = {});

  void print_sequence() const {
    for (auto item : sequence_vector)
      std::cout << item << " ";
    std::cout << std::endl;
  }
};

#endif