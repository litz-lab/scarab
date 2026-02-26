#ifndef DISTRIBUTIONS_H
#define DISTRIBUTIONS_H

#include <cassert>
#include <globals/global_types.h>
#include <random>
#include <vector>

enum Sequence_Pick_Strategy {
  RANDOM,
  NORMAL_ROUND_ROBIN
};

template <typename T, typename Sequence_Type>
class Distribution {
 public:
  template <typename... Args>
  Distribution(Sequence_Pick_Strategy pick_strategy, Args &&...args)
      : _pick_strategy(pick_strategy), _sequence(std::forward<Args>(args)...) {}

  T get_next_element() { return _sequence._get_next_element(_pick_strategy); }
  T get_last_element() { return _sequence._get_last_element(); }

 private:
  Sequence_Pick_Strategy _pick_strategy;
  Sequence_Type _sequence;
};

namespace Sequences {

static std::random_device rd;
static std::mt19937 rand_engine(rd());

template <typename T>
class Strided_Sequence {
 public:
  Strided_Sequence(uns64 size, T stride, T startval)
      : start(startval),
        size(size),
        stride(stride),
        next_element_index(0),
        sequence_vector(size),
        dist(0, size - 1),
        binary_dist(0.5) {
    for (uns64 i{0}; i < size; i++)
      sequence_vector[i] = start + (i * stride);
  }

  T _get_next_element(Sequence_Pick_Strategy strategy) {
    switch (strategy) {
      case NORMAL_ROUND_ROBIN: {
        if (next_element_index >= size)
          next_element_index = 0;
        return sequence_vector[next_element_index++];
      };

      case RANDOM: {
        uns64 index{0};
        if (size == 2)
          index = binary_dist(rand_engine);
        else
          index = dist(Sequences::rand_engine);
        assert((index >= 0 && index < size) && "index must be smaller than size of distribution");
        return sequence_vector[index];
      };

      default:
        return sequence_vector[next_element_index++];
    }
  }

  T _get_last_element() { return *sequence_vector.rbegin(); }

 private:
  T start;
  uns64 size;
  T stride;
  uns64 next_element_index;
  std::vector<T> sequence_vector;
  std::uniform_int_distribution<uns64> dist;
  std::bernoulli_distribution binary_dist; //used if there are only 2 elements in the strided distribution
};

class Boolean_Sequence {
 public:
  Boolean_Sequence(uns64 size, double t_nt_ratio)
      : size(size), 
      next_element_index(0), 
      boolean_array(size), 
      binary_dist(t_nt_ratio) 
  {
    for (auto &item : boolean_array)
      item = static_cast<uns16>(binary_dist(rand_engine));
  }

  bool _get_next_element(Sequence_Pick_Strategy strategy) {
    switch (strategy) {
      case NORMAL_ROUND_ROBIN: {
        if (next_element_index >= size)
          next_element_index = 0;
        return (bool)boolean_array[next_element_index++];
      };

      case RANDOM: {
        return binary_dist(rand_engine);
      };

      default:
        return (bool)boolean_array[next_element_index++];
    }
  }

  bool _get_last_element() { return static_cast<bool>(*boolean_array.rbegin()); }

 private:
  uns64 size;
  uns64 next_element_index;
  std::vector<uns16> boolean_array;
  std::bernoulli_distribution binary_dist;
};

template <typename T>
T genRandomNum(T min, T max) {
  std::uniform_int_distribution<T> dist(min, max);
  return dist(rand_engine);
}

};  // namespace Sequences

#endif