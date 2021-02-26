declare -a benchmarks=("cassandra" "drupal" "finagle-chirper" "finagle-http" "kafka" "mediawiki" "tomcat" "verilator" "wordpress");

sim_binary="/mnt/storage/takh/git-repos/scarab_hlitz/src/bin/Linux/opt/scarab";
params="/mnt/storage/takh/git-repos/scarab_hlitz/test/PARAMS.in";

rm -rf swipe;
mkdir swipe;
cd swipe;
for i in "${benchmarks[@]}";
do
  mkdir $i;
  cd $i;
  for j in `seq 1 100`;
  do
    mkdir $j;
    cd $j;
    cp $params .;
    $sim_binary --frontend pt --cbp_trace_r0=/mnt/storage/takh/pgp/workloads/$i/trace.gz --fetch_off_path_ops=false --inst_limit 49999995 --btb_entries 8192 --btb_assoc 4 --enable_crs 1 --crs_entries 32 --crs_realistic 1 --enable_ibp 1 --use_pat_hist 1 --bp_mech tagescl --btb_mech 2 --fanout $j --warmup 49999995 &> log.txt &
    cd ..;
    if ((j%25==0));
    then
      wait;
    fi
  done
  cd ..;
  wait;
  # ../src/scarab --frontend pt --cbp_trace_r0=/mnt/storage/takh/pgp/workloads/$i/trace.gz --fetch_off_path_ops=false --inst_limit 49999995 --btb_entries 8192 --btb_assoc 4 --enable_crs 1 --crs_entries 32 --crs_realistic 1 --enable_ibp 1 --use_pat_hist 1 --bp_mech tagescl --btb_mech 2 --warmup 49999995 &> pgo_btb/$i/$i.txt; mv *.out pgo_btb/$i/;
done
cd ..;
wait;