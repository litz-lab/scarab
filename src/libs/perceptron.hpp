/* Copyright 2025 Litz Lab
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/***************************************************************************************
 * File         : perceptron.hpp
 * Author       : Naomi Rehman <narehman@ucsc.edu>
 * Date         : 4/29/2025
 * Description  :
 ***************************************************************************************/

#include <numeric>
#include <string>
#include <vector>

#include "globals/assert.h"
#include "globals/global_types.h"

/*
 * Perceptron-based predictor implementation.
 * Core API:
 *   - train: trains weights with given features, the original prediciton, and the correct predicion.
 *   - predict: predicts the outcome based on current weights and given features.
 *   - clear_weights: resets all weights to zero.
 * User-Implementable Policies: (can be overriden with a custom indexing function.)
 *   - Indexing policies: define how to index into the weight table based on the program counter (PC) and history.
 *     Default: PCBasedIndex (uses PC modulo number of entries).
 *   - Counter update policies: define how to update the weights based on the prediction outcome.
 *     Default: SaturatingCounterUpdate (weights saturate at +/-2^(weight_width) - 1).
 * User-Maintained State:
 *   - Features: a vector of doubles representing the features for each prediction.
 *     Features must be extracted and maintained for training by the user.
 *   - Previous prediction and value: used for training.
 *   - Program counter (PC) and history: used for indexing into the weight table.
 */

// INDEXING POLICIES
struct PCBasedIndex {
  static int get_index(Addr pc, int num_entries, uns history) { return (pc % num_entries); }
};

struct PCBasedWithHistory {
  static int get_index(Addr pc, int num_entries, uns history) {
    // Combine PC and history to create a unique index
    return ((pc ^ history) % num_entries);
  }
};

// WEIGHT UPDATE POLICIES
struct SaturatingCounterUpdate {
  template <typename T = double>
  static void weight_update(T& value, double increment, double saturation_value) {
    value += increment;
    if (value > saturation_value) {
      value = saturation_value;
    } else if (value < -saturation_value) {
      value = -saturation_value;
    }
  }
};

struct ContinuousUpdate {
  template <typename T = double>
  static void weight_update(T& value, double increment, double saturation_value) {
    value += increment;
  }
};

struct SingleIncrement {
  template <typename T = double>
  static void weight_update(T& value, double increment, double saturation_value) {
    value += (increment >= 0) ? 1 : -1;
  }
};

template <typename IndexFunction = PCBasedIndex, typename WeightUpdate = SaturatingCounterUpdate>
class PerceptronTable {
 public:
  PerceptronTable(int _num_features, int _num_entries, double _learning_rate, double _theta, int weight_width,
                  double _threshold)
      : num_features(_num_features),
        num_entries(_num_entries),
        learning_rate(_learning_rate),
        theta(_theta),
        saturation_value((int)(1u << weight_width) - 1),
        threshold(_threshold) {
    clear_weights();
  }

  void clear_weights() { weights.assign(num_entries, std::vector<double>(num_features + 1, 0.0)); }

  // Pass prediction from predict, NOT adjusted prediction, for the threshold to work properly
  void train(std::vector<double>& features, bool prediction, bool correct_prediction, double prediction_val, Addr pc,
             uns history) {
    ASSERT(0, features.size() == num_features);
    int index = IndexFunction::get_index(pc, num_entries, history);
    // Using 0 as threshold here is intended. Using threshold in both train and test doesn't move the decision boundary.
    double error = (prediction_val >= 0.0) == correct_prediction ? 0.0 : (correct_prediction ? 1.0 : -1.0);
    if (error == 0.0 && !(prediction_val < theta && prediction > -theta)) {
      return;
    }
    for (u_int i = 0; i < num_features; ++i) {
      WeightUpdate::weight_update(weights[index][i], learning_rate * features[i] * error, saturation_value);
    }
    WeightUpdate::weight_update(weights[index][num_features], learning_rate * error, saturation_value);
  }

  bool predict(std::vector<double>& features, double& sum_out, bool& prediction, Addr pc, uns history) const {
    ASSERT(0, features.size() == num_features);
    int index = IndexFunction::get_index(pc, num_entries, history);
    double sum = std::inner_product(weights[index].begin(), weights[index].end() - 1, features.begin(),
                                    weights[index][num_features]);
    sum_out = sum;
    prediction = sum >= threshold;
    return prediction;
  }

  void print_weights(FILE* fp, std::string feature_names) const {
    fprintf(fp, "Index  Bias  %s", feature_names.c_str());
    for (u_int i = 0; i < num_entries; ++i) {
      fprintf(fp, "%d,", i);
      for (u_int j = 0; j < num_features + 1; ++j) {
        fprintf(fp, "%f,", weights[i][j]);
      }
      fprintf(fp, "\n");
    }
  }

  // helpful functions
  static void extract_bipolar_features(int input, double* output, int num_features) {
    for (u_int i = 0; i < num_features; ++i) {
      output[i] = (input & (1 << i)) ? 1.0 : -1.0;
    }
  }

  static void extract_binary_features(int input, double* output, int num_features) {
    for (u_int i = 0; i < num_features; ++i) {
      output[i] = (input & (1 << i)) ? 1.0 : 0.0;
    }
  }

 private:
  std::vector<std::vector<double>> weights;
  unsigned int num_features;
  unsigned int num_entries;
  double learning_rate;
  double theta;
  double saturation_value;
  double threshold;  // moves decision boundary for inference
};