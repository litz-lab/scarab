/* Copyright 2020 HPS/SAFARI Research Groups
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
* File         : statistics.c
* Author       : HPS Research Group
* Date         : 2/11/1998
* Description  : This file handles the statistics for scarab.  Statistics are
declared in the various '.stat.def' files and included for form an enum.  Events
are tracked using the STAT_EVENT macros given in the header file.  I'm still
working on this (ob).
***************************************************************************************/
#include "statistics.h"

#include <math.h>

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "core.param.h"
#include "general.param.h"

#include "optimizer2.h"

/**************************************************************************************/
/* Global Variables */

#define DEF_STAT(name, type, ratio) {type##_TYPE_STAT, #name, {0}, {0}, ratio, __FILE__, FALSE},

Stat global_stat_sample[] = {
#include "stat_files.def"
};

#undef DEF_STAT

Stat** global_stat_array;

/**************************************************************************************/
// init_global_stats_array:
void init_global_stats_array() {
  uns ii;
  // Fix file_name in each stat to be just the filename without any
  // path information. This fix is needed to enable clang
  // compilation because clang replaces __FILE__ with
  // './file.stat.def' instead of 'file.stat.def', breaking
  // composite "filetag-filename" file name construction
  for (ii = 0; ii < NUM_GLOBAL_STATS; ii++) {
    Stat* stat = &global_stat_sample[ii];
    const char* last_slash = strrchr(stat->file_name, '/');
    if (last_slash)
      stat->file_name = last_slash + 1;
  }

  // Make a copy of stats array for each core
  global_stat_array = (Stat**)malloc(NUM_CORES * sizeof(Stat*));
  for (ii = 0; ii < NUM_CORES; ii++) {
    global_stat_array[ii] = (Stat*)malloc(NUM_GLOBAL_STATS * sizeof(Stat));
    memcpy(global_stat_array[ii], global_stat_sample, NUM_GLOBAL_STATS * sizeof(Stat));
  }
}

/**************************************************************************************/
// gen_stat_output_file:

void gen_stat_output_file(char* buf, uns8 proc_id, Stat* stat, char csv) {
  char temp[MAX_STR_LENGTH + 1];
  char temp2[16];  // assuming proc id can not be more than 15 bytes

  /* prepend the stat tag, cut off the 'def' ending and add 'out' */
  // strncpy(temp, stat->file_name, strlen(stat->file_name) - 3);
  strncpy(temp, stat->file_name, MAX_STR_LENGTH);
  temp[strlen(stat->file_name) - 3] = '\0';
  sprintf(temp2, "%u", proc_id);
  strncat(temp, temp2, MAX_STR_LENGTH);

  if (csv)
    strncat(temp, ".csv", MAX_STR_LENGTH);
  else
    strncat(temp, ".out", MAX_STR_LENGTH);

  if (PERIODIC_DUMP) {
    char temp3[24];
    sprintf(temp3, ".period.%llu", period_ID);
    strncat(temp, temp3, 24);
  }
  if (FULL_WARMUP && !warmup_dump_done[proc_id]) {
    char temp3[24];
    sprintf(temp3, ".warmup");
    strncat(temp, temp3, 24);
  }
  if (roi_dump_began) {
    char temp3[24];
    sprintf(temp3, ".roi.%llu", roi_dump_ID);
    strncat(temp, temp3, 24);
  }
  strncpy(buf, OUTPUT_DIR, MAX_STR_LENGTH);
  strncat(buf, "/", MAX_STR_LENGTH);
  strncat(buf, FILE_TAG, MAX_STR_LENGTH);
  strncat(buf, temp, MAX_STR_LENGTH + 1);
}

/**************************************************************************************/
/* init_stats: */

void init_global_stats(uns8 proc_id) {
  uns ii;

  for (ii = 0; ii < NUM_GLOBAL_STATS; ii++) {
    Stat* stat = &global_stat_array[proc_id][ii];
    const char* noreset_prefix = "NORESET";
    const char* param_prefix = "PARAM";
    if (!strncmp(stat->name, noreset_prefix, strlen(noreset_prefix)) ||
        !strncmp(stat->name, param_prefix, strlen(param_prefix))) {
      stat->noreset = TRUE;
    }
  }
}

/**************************************************************************************/
/* fprint_line: */

void fprint_line(FILE* file) {
  fprintf(file, "##################################################");
  fprintf(file, "##################################################\n");
}

/**************************************************************************************/
/* dump_stats: */

void dump_stats(uns8 proc_id, Flag final, Stat stat_array[], uns num_stats) {
  Flag in_dist = FALSE;

  uns64 dist_sum = 0, total_dist_sum = 0, dist_vtotal = 0, total_dist_vtotal = 0;
  double dist_variance = 0, total_dist_variance = 0;
  uns ii;

  if (!DUMP_STATS)
    return;

  for (ii = 0; ii < num_stats; ii++) {
    Stat* s = &stat_array[ii];

    /* update the total counter for this interval */
    if (s->type == FLOAT_TYPE_STAT)
      s->total_value += s->value;
    else
      s->total_count += s->count;
  }

  const char* last_file_name = NULL;
  FILE* file_stream = NULL;
  FILE* csv_file_stream = NULL;

  uns stat_groupname = 0;
  const static uns STATISTICS_CSV_NO_GROUP = 0;

  for (ii = 0; ii < num_stats; ii++) {
    Stat* s = &stat_array[ii];

    if (!last_file_name || s->file_name != last_file_name) {
      if (last_file_name) {
        ASSERT(0, file_stream);
        fprintf(file_stream, "\n\n");
        fclose(file_stream);
        file_stream = NULL;

        ASSERT(0, csv_file_stream);
        fprintf(csv_file_stream, "\n\n");
        fclose(csv_file_stream);
        csv_file_stream = NULL;
      }
      last_file_name = s->file_name;
      ASSERT(0, !file_stream);
      char buf[MAX_STR_LENGTH + 2];
      gen_stat_output_file(buf, proc_id, s, 0);
      file_stream = fopen(buf, "w");
      ASSERTUM(0, file_stream, "Couldn't open statistic output file '%s'.\n", buf);

      ASSERT(0, !csv_file_stream);
      char csv_buf[MAX_STR_LENGTH + 2];
      gen_stat_output_file(csv_buf, proc_id, s, 1);
      csv_file_stream = fopen(csv_buf, "w");
      ASSERTUM(0, csv_file_stream, "Couldn't open statistic output file '%s'.\n", csv_buf);

      // .out file
      fprintf(file_stream, "/* -*- Mode: c -*- */\n");
      fprint_line(file_stream);
      fprintf(file_stream, "Core %u\n", proc_id);
      fprint_line(file_stream);

      fprintf(file_stream,
              "Cumulative:        Cycles: %-20llu  Instructions: %-20llu  IPC: "
              "%.5f\n",
              cycle_count, inst_count[proc_id], (double)inst_count[proc_id] / cycle_count);
      fprintf(file_stream, "\n");

      fprintf(
          file_stream,
          "Periodic:          Cycles: %-20llu  Instructions: %-20llu  IPC: "
          "%.5f\n",
          cycle_count - period_last_cycle_count, inst_count[proc_id] - period_last_inst_count[proc_id],
          (double)(inst_count[proc_id] - period_last_inst_count[proc_id]) / (cycle_count - period_last_cycle_count));
      fprintf(file_stream, "\n");

      //.csv file
      fprintf(csv_file_stream, "Core, %d, %u\n", STATISTICS_CSV_NO_GROUP, proc_id);

      fprintf(csv_file_stream, "Cumulative_Cycles, %d, %-20llu\nCumulative_Instructions, %d, %-20llu\n",
              STATISTICS_CSV_NO_GROUP, cycle_count, STATISTICS_CSV_NO_GROUP, inst_count[proc_id]);

      fprintf(csv_file_stream, "Periodic_Cycles, %d, %-20llu\nPeriodic_Instructions, %d, %-20llu\n",
              STATISTICS_CSV_NO_GROUP, cycle_count - period_last_cycle_count, STATISTICS_CSV_NO_GROUP,
              inst_count[proc_id] - period_last_inst_count[proc_id]);
    }

    if (s->type == LINE_TYPE_STAT) {
      fprintf(file_stream, "\n/*******************************************");
      fprintf(file_stream, "*******************************************/\n");
    }

    fprintf(file_stream, "%-40s ", s->name);

    switch (s->type) {
      case COUNT_TYPE_STAT:
        if (!in_dist) {
          fprintf(file_stream, "%13s %13s    %13s %13s\n", unsstr64(s->count), "", unsstr64(s->total_count), "");

          fprintf(csv_file_stream, "%s_count, %d, %13s\n", s->name, STATISTICS_CSV_NO_GROUP, unsstr64(s->count));
          fprintf(csv_file_stream, "%s_total_count, %d, %13s\n", s->name, STATISTICS_CSV_NO_GROUP,
                  unsstr64(s->total_count));
        } else {
          fprintf(file_stream, "%13s %12.3f%%    %13s %12.3f%%", unsstr64(s->count), (double)s->count / dist_sum * 100,
                  unsstr64(s->total_count), (double)s->total_count / total_dist_sum * 100);

          // Dist percentages calculation offloaded to python
          fprintf(csv_file_stream, "%s_count, %d, %13s\n", s->name, stat_groupname, unsstr64(s->count));
          fprintf(csv_file_stream, "%s_total_count, %d, %13s\n", s->name, stat_groupname, unsstr64(s->total_count));
        }
        break;

      case FLOAT_TYPE_STAT:
        ASSERTM(0, !in_dist, "Distributions not supported for float stats\n");
        fprintf(file_stream, "%13lf %13s    %13lf %13s\n", s->value, "", s->total_value, "");

        fprintf(csv_file_stream, "%s_value, %d, %13lf\n", s->name, STATISTICS_CSV_NO_GROUP, s->value);
        fprintf(csv_file_stream, "%s_total_value, %d, %13lf\n", s->name, STATISTICS_CSV_NO_GROUP, s->total_value);
        break;

      case DIST_TYPE_STAT:
        if (!in_dist) {
          stat_groupname++;

          uns jj;

          in_dist = TRUE;
          dist_sum = s->count;
          total_dist_sum = s->total_count;
          dist_vtotal = 0;
          total_dist_vtotal = 0;

          for (jj = ii + 1; stat_array[jj].type != DIST_TYPE_STAT; jj++) {
            dist_sum += stat_array[jj].count;
            total_dist_sum += stat_array[jj].total_count;
            dist_vtotal += (jj - ii) * stat_array[jj].count;
            total_dist_vtotal += (jj - ii) * stat_array[jj].total_count;
          }
          dist_sum += stat_array[jj].count;
          total_dist_sum += stat_array[jj].total_count;
          dist_vtotal += (jj - ii) * stat_array[jj].count;
          total_dist_vtotal += (jj - ii) * stat_array[jj].total_count;

          dist_variance = pow((0.0 - ((double)dist_vtotal / dist_sum)), 2) * stat_array[jj].count;
          total_dist_variance =
              pow((0.0 - ((double)total_dist_vtotal / total_dist_sum)), 2) * stat_array[jj].total_count;
          for (jj = ii + 1; stat_array[jj].type != DIST_TYPE_STAT; jj++) {
            dist_variance += pow((jj - ii - ((double)dist_vtotal / dist_sum)), 2) * stat_array[jj].count;
            total_dist_variance +=
                pow((jj - ii - ((double)total_dist_vtotal / total_dist_sum)), 2) * stat_array[jj].total_count;
          }
          dist_variance += pow((jj - ii - ((double)dist_vtotal / dist_sum)), 2) * stat_array[jj].count;
          total_dist_variance +=
              pow((jj - ii - ((double)total_dist_vtotal / total_dist_sum)), 2) * stat_array[jj].total_count;
          dist_variance /= dist_sum - 1;
          total_dist_variance /= total_dist_sum - 1;

          fprintf(file_stream, "%13s %12.3f%%    %13s %12.3f%%", unsstr64(s->count), (double)s->count / dist_sum * 100,
                  unsstr64(s->total_count), (double)s->total_count / total_dist_sum * 100);

          // DIST pct offloaded to python
          fprintf(csv_file_stream, "%s_count, %d, %13s\n", s->name, stat_groupname, unsstr64(s->count));
          fprintf(csv_file_stream, "%s_total_count, %d, %13s\n", s->name, stat_groupname, unsstr64(s->total_count));
        } else {
          in_dist = FALSE;
          fprintf(file_stream, "%13s %12.3f%%    %13s %12.3f%%\n", unsstr64(s->count),
                  (double)s->count / dist_sum * 100, unsstr64(s->total_count),
                  (double)s->total_count / total_dist_sum * 100);

          // DIST pct offloaded to python
          fprintf(csv_file_stream, "%s_count, %d, %13s\n", s->name, stat_groupname, unsstr64(s->count));
          fprintf(csv_file_stream, "%s_total_count, %d, %13s\n", s->name, stat_groupname, unsstr64(s->total_count));

          // print sum information
          fprintf(file_stream, "%-40s %13s %12.3f%%    %13s %12.3f%%\n", "", unsstr64(dist_sum),
                  (double)dist_sum / dist_sum * 100, unsstr64(total_dist_sum),
                  (double)total_dist_sum / total_dist_sum * 100);

          // print index amean and stddev
          fprintf(file_stream, "%-40s  %12.2f %12.2f      %12.2f %12.2f\n", "", (double)dist_vtotal / dist_sum,
                  sqrt(dist_variance), (double)total_dist_vtotal / total_dist_sum, sqrt(total_dist_variance));
        }
        break;

      case PER_INST_TYPE_STAT:
        fprintf(file_stream, "%13s %13.4f    %13s %13.4f\n", unsstr64(s->count),
                (double)s->count / (double)inst_count[proc_id], unsstr64(s->total_count),
                (double)s->total_count / (double)inst_count[proc_id]);

        fprintf(csv_file_stream, "%s_count, %d, %13s\n", s->name, STATISTICS_CSV_NO_GROUP, unsstr64(s->count));
        fprintf(csv_file_stream, "%s_pct, %d, %12.3f\n", s->name, STATISTICS_CSV_NO_GROUP,
                (double)s->count / (double)inst_count[proc_id]);
        fprintf(csv_file_stream, "%s_total_count, %d, %13s\n", s->name, STATISTICS_CSV_NO_GROUP,
                unsstr64(s->total_count));
        fprintf(csv_file_stream, "%s_total_pct, %d, %12.3f\n", s->name, STATISTICS_CSV_NO_GROUP,
                (double)s->total_count / (double)inst_count[proc_id]);
        break;

      case PER_1000_INST_TYPE_STAT:
        fprintf(file_stream, "%13s %13.4f    %13s %13.4f\n", unsstr64(s->count),
                (double)1000.0 * (double)s->count / (double)inst_count[proc_id], unsstr64(s->total_count),
                (double)1000.0 * (double)s->total_count / (double)inst_count[proc_id]);

        fprintf(csv_file_stream, "%s_count, %d, %13s\n", s->name, STATISTICS_CSV_NO_GROUP, unsstr64(s->count));
        fprintf(csv_file_stream, "%s_pct, %d, %12.3f\n", s->name, STATISTICS_CSV_NO_GROUP,
                (double)1000.0 * (double)s->count / (double)inst_count[proc_id]);
        fprintf(csv_file_stream, "%s_total_count, %d, %13s\n", s->name, STATISTICS_CSV_NO_GROUP,
                unsstr64(s->total_count));
        fprintf(csv_file_stream, "%s_total_pct, %d, %12.3f\n", s->name, STATISTICS_CSV_NO_GROUP,
                (double)1000.0 * (double)s->total_count / (double)inst_count[proc_id]);
        break;

      case PER_1000_PRET_INST_TYPE_STAT:
        fprintf(file_stream, "%13s %13.4f    %13s %13.4f\n", unsstr64(s->count),
                (double)1000.0 * (double)s->count / (double)pret_inst_count[proc_id], unsstr64(s->total_count),
                (double)1000.0 * (double)s->total_count / (double)pret_inst_count[0]);

        fprintf(csv_file_stream, "%s_count, %d, %13s\n", s->name, STATISTICS_CSV_NO_GROUP, unsstr64(s->count));
        fprintf(csv_file_stream, "%s_pct, %d, %12.3f\n", s->name, STATISTICS_CSV_NO_GROUP,
                (double)1000.0 * (double)s->count / (double)pret_inst_count[proc_id]);
        fprintf(csv_file_stream, "%s_total_count, %d, %13s\n", s->name, STATISTICS_CSV_NO_GROUP,
                unsstr64(s->total_count));
        fprintf(csv_file_stream, "%s_total_pct, %d, %12.3f\n", s->name, STATISTICS_CSV_NO_GROUP,
                (double)1000.0 * (double)s->total_count / (double)pret_inst_count[0]);
        break;

      case PER_CYCLE_TYPE_STAT:
        fprintf(file_stream, "%13s %13.4f    %13s %13.4f\n", unsstr64(s->count), (double)s->count / (double)cycle_count,
                unsstr64(s->total_count), (double)s->total_count / (double)cycle_count);

        fprintf(csv_file_stream, "%s_count, %d, %13s\n", s->name, STATISTICS_CSV_NO_GROUP, unsstr64(s->count));
        fprintf(csv_file_stream, "%s_pct, %d, %12.3f\n", s->name, STATISTICS_CSV_NO_GROUP,
                (double)s->count / (double)cycle_count);
        fprintf(csv_file_stream, "%s_total_count, %d, %13s\n", s->name, STATISTICS_CSV_NO_GROUP,
                unsstr64(s->total_count));
        fprintf(csv_file_stream, "%s_total_pct, %d, %12.3f\n", s->name, STATISTICS_CSV_NO_GROUP,
                (double)s->total_count / (double)cycle_count);
        break;

      case RATIO_TYPE_STAT:
        fprintf(file_stream, "%13s %13.4f    %13s %13.4f\n", unsstr64(s->count),
                (double)s->count / (double)(stat_array[s->ratio_stat].count), unsstr64(s->total_count),
                (double)s->total_count / (double)stat_array[s->ratio_stat].total_count);

        fprintf(csv_file_stream, "%s_count, %d, %13s\n", s->name, STATISTICS_CSV_NO_GROUP, unsstr64(s->count));
        fprintf(csv_file_stream, "%s_pct, %d, %12.3f\n", s->name, STATISTICS_CSV_NO_GROUP,
                (double)s->count / (double)(stat_array[s->ratio_stat].count));
        fprintf(csv_file_stream, "%s_total_count, %d, %13s\n", s->name, STATISTICS_CSV_NO_GROUP,
                unsstr64(s->total_count));
        fprintf(csv_file_stream, "%s_total_pct, %d, %12.3f\n", s->name, STATISTICS_CSV_NO_GROUP,
                (double)s->total_count / (double)stat_array[s->ratio_stat].total_count);
        break;

      case PERCENT_TYPE_STAT:
        fprintf(file_stream, "%13s %12.3f%%    %13s %12.3f%%\n", unsstr64(s->count),
                (double)s->count * 100 / (double)(stat_array[s->ratio_stat].count), unsstr64(s->total_count),
                (double)s->total_count * 100 / (double)stat_array[s->ratio_stat].total_count);

        fprintf(csv_file_stream, "%s_count, %d, %13s\n", s->name, STATISTICS_CSV_NO_GROUP, unsstr64(s->count));
        fprintf(csv_file_stream, "%s_pct, %d, %12.3f\n", s->name, STATISTICS_CSV_NO_GROUP,
                (double)s->count * 100 / (double)(stat_array[s->ratio_stat].count));
        fprintf(csv_file_stream, "%s_total_count, %d, %13s\n", s->name, STATISTICS_CSV_NO_GROUP,
                unsstr64(s->total_count));
        fprintf(csv_file_stream, "%s_total_pct, %d, %12.3f\n", s->name, STATISTICS_CSV_NO_GROUP,
                (double)s->total_count * 100 / (double)stat_array[s->ratio_stat].total_count);
        break;

      case LINE_TYPE_STAT:
        fprintf(file_stream, "\n/*******************************************");
        fprintf(file_stream, "*******************************************/\n");
        break;

      default:
        FATAL_ERROR(0, "Invalid statistic type.\n");
    }

    fprintf(file_stream, "\n");
    fprintf(csv_file_stream, "\n");
    fflush(file_stream);
    fflush(csv_file_stream);
  }

  if (last_file_name) {
    ASSERT(0, file_stream);
    fprintf(file_stream, "\n\n");
    fclose(file_stream);
    file_stream = NULL;

    ASSERT(0, csv_file_stream);
    fprintf(csv_file_stream, "\n\n");
    fclose(csv_file_stream);
    csv_file_stream = NULL;
  }

  /* reset the interval counters */
  for (ii = 0; ii < num_stats; ii++) {
    Stat* s = &stat_array[ii];
    if (s->type == FLOAT_TYPE_STAT)
      s->value = 0.0;
    else
      s->count = 0;
  }
}

/**************************************************************************************/
/* dump_stats: */

void reset_stats(Flag keep_total) {
  uns proc_id, ii;
  if (!opt2_in_use() || opt2_is_leader()) {
    fprintf(mystdout, "** Stats Cleared:   insts: { ");
    for (proc_id = 0; proc_id < NUM_CORES; proc_id++)
      fprintf(mystdout, "%lld ", inst_count[proc_id]);
    fprintf(mystdout, "}  cycles: %-10s  time %-18s\n", unsstr64(cycle_count), unsstr64(sim_time));
    fflush(mystdout);
  }

  for (ii = 0; ii < NUM_GLOBAL_STATS; ii++) {
    for (proc_id = 0; proc_id < NUM_CORES; proc_id++) {
      Stat* stat = &global_stat_array[proc_id][ii];
      if (stat->type == FLOAT_TYPE_STAT) {
        if (keep_total || stat->noreset)
          stat->total_value += stat->value;
        stat->value = 0.0;
      } else {
        if (keep_total || stat->noreset)
          stat->total_count += stat->count;
        stat->count = 0ULL;
      }
    }
  }
}

/**************************************************************************************/
/* get_stat_idx: */

Stat_Enum get_stat_idx(const char* name) {
  uns ii;
  for (ii = 0; ii < NUM_GLOBAL_STATS; ii++) {
    Stat* stat = &global_stat_array[0][ii];
    if (!strcmp(stat->name, name))
      break;
  }
  return ii;  // equals NUM_GLOBAL_STATS if stat not found
}

/**************************************************************************************/
/* get_stat: */

const Stat* get_stat(uns8 proc_id, const char* name) {
  ASSERT(0, proc_id < NUM_CORES);
  Stat_Enum idx = get_stat_idx(name);
  if (idx == NUM_GLOBAL_STATS)
    return NULL;
  return &global_stat_array[proc_id][idx];
}

/**************************************************************************************/
/* get_stat: */

Counter get_accum_stat_event(Stat_Enum name) {
  Counter accum = 0;

  if (name == NUM_GLOBAL_STATS)
    return 0;

  for (uns proc_id = 0; proc_id < NUM_CORES; proc_id++) {
    accum += global_stat_array[proc_id][name].total_count;
  }

  return accum;
}
