
#include "sampler.h"

#include <algorithm>
#include <cassert>
/*
 * Overloaded Constructor that generates based on
 * value_range : number of discrete values in the sequence(normal_random) or sequence_length(uniform sequences).
 * start_val   : is the beginning element ofthe sequence
 * stride      : stride between elements in the sequence
 * periodicity : number of sequence duplicates
 * strategy    : suggests the pattern of the sequence
 */

Sampler::Sampler(Sequence_Pick_Strategy strategy, uns64 value_range, uns64 startval, uns64 stride, uns periodicity,
                 std::vector<uns64> user_seq)
    : strategy(strategy), next_pick_index(0) {
  switch (strategy) {
    case UNIFORM_SEQUENTIAL:
    default:  // <- default
      sequence_vector.resize(value_range);
      // cr eate sequence
      for (uns64 i{0}; i < value_range; i++) {
        sequence_vector[i] = startval + i * stride;
      }
      Sampler::scale_periodicity(periodicity);
      break;

    case UNIFORM_RANDOM:
      sequence_vector.resize(value_range);
      // create sequence
      for (uns64 i{0}; i < value_range; i++) {
        sequence_vector[i] = startval + i * stride;
      };
      // shuffle
      std::shuffle(sequence_vector.begin(), sequence_vector.end(), rng_engine);
      Sampler::scale_periodicity(periodicity);
      break;

    case NORMAL_RANDOM: {
      uns64 H = 90;                   // scaling factor
      uns64 mu = value_range / 2;     // center
      uns64 sigma = value_range / 6;  // spread

      std::vector<uns64> discrete_weights(value_range);  // weights of discrete
                                                         // values
      std::vector<uns64> discrete_values(value_range);   // set of distinct values going to be in the sequence

      // generate the set of discrete values
      for (uns64 i{0}; i < value_range; i++) {
        discrete_values[i] = startval + i * stride;
      };

      // generate weights for each discrete value
      for (uns64 i = 0; i < value_range; i++)
        discrete_weights[i] = static_cast<uns64>(std::round(H * std::exp(-0.5 * std::pow((i - mu) / sigma, 2))));

      // duplicate discrete values according to their weights
      for (uns64 i = 0; i < value_range; i++) {
        for (uns64 j = 0; j < discrete_weights[i]; j++) {
          sequence_vector.push_back(discrete_values[i]);
        }
      }
      // shuffle distribution
      std::shuffle(sequence_vector.begin(), sequence_vector.end(), rng_engine);
      Sampler::scale_periodicity(periodicity);
      break;
    }

    case USER_DEFINED:
      sequence_vector = user_seq;
      Sampler::scale_periodicity(periodicity);
      break;
  }
}

/*
 * Overloaded Constructor receives a userdefined sequence set of discreete
 values and constructs a sequence based on selection strategy.
 * discrete_values  : vecctor of distinct values that are going to be in the
 sequence.
 * discrete_weights : corresponding weights of distinct values, by default all
 are equally likely, ie; = 1.
 * sequence_length  : specifies length of the discrete distribution to be
 generated.
 * periodicity      : number of sequence duplicates
 * strategy         : suggests the pattern of the sequence
 NB: when strategy is UNIFORM_*, the discrete_values become the actual sequence
 to be consumed, if NORMAL_RANDOM we generate a discrete sequence based on the
 dicrete values and their corresponding weigths
 */

Sampler::Sampler(std::vector<uns64> discrete_values, Sequence_Pick_Strategy strategy, uns64 periodicity,
                 uns sequence_length, std::vector<uns64> discrete_weights = {})
    : strategy(strategy), next_pick_index(0) {
  assert(!discrete_values.empty() && "sequence cannot be empty");
  switch (strategy) {
    case UNIFORM_SEQUENTIAL:
    case USER_DEFINED:
    default:  // <- default
      sequence_vector = discrete_values;
      Sampler::scale_periodicity(periodicity);
      break;
    case UNIFORM_RANDOM:
      sequence_vector = discrete_values;
      std::shuffle(sequence_vector.begin(), sequence_vector.end(), rng_engine);
      Sampler::scale_periodicity(periodicity);
      break;
    case NORMAL_RANDOM:
      if (discrete_weights.empty()) {
        // if user didnt pass weights, every unique sample has equal weight
        discrete_weights = std::vector<uns64>(discrete_values.size(), 1);
      }
      // RNG based on discrete weights
      std::discrete_distribution<uns64> discrete_dist(discrete_weights.begin(), discrete_weights.end());

      for (size_t i{0}; i < sequence_length; i++) {
        auto idx = discrete_dist(rng_engine);
        assert(idx < discrete_values.size() && "distribution exceeds index");
        sequence_vector.push_back(discrete_values[idx]);
      }
      Sampler::scale_periodicity(periodicity);
      break;
  }
}

uns64 Sampler::get_next_element() {
  auto index{next_pick_index};
  if (++next_pick_index >= sequence_vector.size())
    next_pick_index = 0;
  return sequence_vector[index];
}
// seed
static std::random_device seed;
// Define and initialize outside the class
std::default_random_engine Sampler::rng_engine(seed());

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