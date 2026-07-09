
#include "sampler.h"

#include <algorithm>
#include <cassert>
uns64 Sampler::get_next_element() {
  auto index{next_pick_index};
  if (++next_pick_index >= sequence_vector.size())
    next_pick_index = 0;
  return sequence_vector[index];
}

void Sampler::scale_periodicity(uns64 periodicity) {
  auto original_vector = Sampler::sequence_vector;
  sequence_vector.reserve(sequence_vector.size() * periodicity);
  for (uns i = 1; i < periodicity; i++) {
    sequence_vector.insert(sequence_vector.end(), original_vector.begin(), original_vector.end());
  }
}

uns64 Sampler::peek_element_following_next() const {
  auto index = (next_pick_index + 1) % sequence_vector.size();
  if (index < next_pick_index)
    return sequence_vector.back();
  else
    return sequence_vector[index];
}

// generates a linear sequence vector
std::vector<uns64> Sampler::generate_range_based_sequence(uns64 value_range, uns64 startval, uns64 stride) {
  std::vector<uns64> sequence_vector(value_range);
  for (uns64 i{0}; i < value_range; i++) {
    sequence_vector[i] = startval + i * stride;
  };
  return sequence_vector;
}
// generates a sequence vector based on normal curve (gaussian)
std::vector<uns64> Sampler::generate_normal_curve_sequence(uns64 value_range, uns64 startval, uns64 stride) {
  std::vector<uns64> sequence_vector;
  uns64 H = 90;                   // scaling factor
  uns64 mu = value_range / 2;     // center
  uns64 sigma = value_range / 6;  // spread
  uns64 element = 0;
  uns64 weight = 0;

  for (uns64 i = 0; i < value_range; i++) {
    // generate a distinct element of the normal distribution
    element = startval + i * stride;
    // generate a corresponding weight for element according to the normal distribution
    weight = static_cast<uns64>(std::round(H * std::exp(-0.5 * std::pow((i - mu) / sigma, 2))));
    // generate j(weight) samples according
    for (uns64 j = 0; j < weight; j++) {
      sequence_vector.push_back(element);
    }
  }
  return sequence_vector;
}
// generates a sequence vector based on user provided sequence and weights
std::vector<uns64> Sampler::generate_discrete_weight_sequence(std::vector<uns64> values, uns sequence_length,
                                                              std::vector<uns64> weights) {
  // all elements in the sequence have the same weight if weights are not specified
  if (weights.empty()) {
    weights = std::vector<uns64>(values.size(), 1);
  }

  std::discrete_distribution<uns64> discrete_dist(weights.begin(), weights.end());
  std::vector<uns64> sequence_vector;
  sequence_vector.reserve(sequence_length);

  for (uns i{0}; i < sequence_length; i++) {
    uns index = discrete_dist(rng_engine);
    assert(index <= values.size());
    sequence_vector.push_back(values[index]);
  }

  return sequence_vector;
}

// Common Constructor...shared code by all sequence types
Sampler::Sampler(std::vector<uns64> seq_vec, Sequence_Pick_Strategy strategy, uns periodicity, bool should_shuffle)
    : sequence_vector(std::move(seq_vec)), strategy(strategy), next_pick_index(0), rng_engine(seed()) {
  if (should_shuffle)
    std::shuffle(sequence_vector.begin(), sequence_vector.end(), rng_engine);
  scale_periodicity(periodicity);
}

// Builds UNIFORM_SEQUENTIAL, NORMAL_RANDOM or UNIFORM_RANDOM sequence and passes control to common constructor
Sampler::Sampler(Sequence_Pick_Strategy strategy, uns periodicity, uns64 value_range, uns64 startval, uns64 stride)
    : Sampler((strategy == UNIFORM_RANDOM || strategy == UNIFORM_SEQUENTIAL)
                  ? generate_range_based_sequence(value_range, startval, stride)
                  : generate_normal_curve_sequence(value_range, startval, stride),
              strategy, periodicity, (strategy == UNIFORM_RANDOM || strategy == NORMAL_RANDOM)) {
  assert((strategy == Sequence_Pick_Strategy::UNIFORM_RANDOM || strategy == Sequence_Pick_Strategy::NORMAL_RANDOM ||
          strategy == Sequence_Pick_Strategy::UNIFORM_SEQUENTIAL) &&
         "this constructor builds either uniform_random or normal_random or uniform sequential");
}

// Builds DISCRETE_RANDOM or retains USER_DEFINED sequence vector and passes control to common constructor
Sampler::Sampler(Sequence_Pick_Strategy strategy, uns periodicity, std::vector<uns64> values, uns sequence_length,
                 std::vector<uns64> weights)
    : Sampler((strategy == Sequence_Pick_Strategy::DICRETE_RANDOM
                   ? generate_discrete_weight_sequence(values, sequence_length, weights)
                   : values),
              strategy, periodicity, (strategy == Sequence_Pick_Strategy::DICRETE_RANDOM ? true : false)) {
  assert((strategy == Sequence_Pick_Strategy::DICRETE_RANDOM || strategy == Sequence_Pick_Strategy::USER_DEFINED) &&
         "this constructor only builds discrete random or user_Defined sequences");
}
