rm -rf drmemtrace*
$DRIO_ROOT/build/bin64/drrun -t drcachesim -offline -- /bin/ls
a=$(realpath drmemtrace*)
$DRIO_ROOT/build/bin64/drrun -t drcachesim -indir $a
cp ../src/PARAMS.kaby_lake PARAMS.in
../src/scarab --frontend memtrace --cbp_trace_r0=$a/trace --memtrace_modules_log=$a/raw --fetch_off_path_ops false --inst_limit 10000 # 1000000 causes an error possibly because of an unsupported instruction
