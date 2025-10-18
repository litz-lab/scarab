#!/bin/bash

# config
INST_NUM=1000000
OUTPUT_DIR=$(pwd)/../output
APP_NAME=gcc
LOAD_VALUE_PRED_SCHEME=1
CONST_LOAD_ADDR_PRED_THRESHOLD=100

# trace
case $APP_NAME in
  gcc)
    TRACEFILE=/path/to/trace
    ;;
  *)
    echo "unknown application"
    ;;
esac

# option
SCARAB_OPTION="
--frontend=memtrace \
--output_dir=$OUTPUT_DIR \
--inst_limit=$INST_NUM \
--cbp_trace_r0=$TRACEFILE \
--reg_renaming_scheme=0 \
--lsq_enable=0 \
--load_value_pred_scheme=$LOAD_VALUE_PRED_SCHEME \
--const_load_addr_pred_threshold=$CONST_LOAD_ADDR_PRED_THRESHOLD
"

# run the cmd
mkdir -p ../output
if [[ "$1" == "log" ]]; then
  ./scarab $SCARAB_OPTION &> _log_sim.txt
else
  ./scarab $SCARAB_OPTION
fi

