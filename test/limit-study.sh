declare -a benchmarks=("cassandra" "drupal" "finagle-chirper" "finagle-http" "kafka" "mediawiki" "tomcat" "verilator" "wordpress");

sim_binary="/mnt/storage/takh/git-repos/scarab_hlitz/src/bin/Linux/opt/scarab";
params="/mnt/storage/takh/git-repos/scarab_hlitz/test/PARAMS.in";

rm -rf limit-study;
mkdir limit-study;
cd limit-study;
for i in "${benchmarks[@]}";
do
  mkdir $i;
  cd $i;
  
  mkdir baseline;
  cd baseline;
  cp $params .;
  $sim_binary --frontend pt --cbp_trace_r0=/mnt/storage/takh/pgp/workloads/$i/trace.gz --fetch_off_path_ops=false --inst_limit 99999990 --btb_entries 8192 --btb_assoc 4 --enable_crs 1 --crs_entries 32 --enable_ibp 1 --crs_realistic 1 --use_pat_hist 1 --bp_mech tagescl --btb_mech 0 --fdip_enable 1 --fdip_max_runahead 64 --fdip_break_icache 1 --fdip_nlp 1 &> log.txt & # 
  cd ..;
  
  mkdir perfect_icache;
  cd perfect_icache;
  cp $params .;
  $sim_binary --frontend pt --cbp_trace_r0=/mnt/storage/takh/pgp/workloads/$i/trace.gz --fetch_off_path_ops=false --inst_limit 99999990 --btb_entries 8192 --btb_assoc 4 --enable_crs 1 --crs_entries 32 --enable_ibp 1 --crs_realistic 1 --use_pat_hist 1 --bp_mech tagescl --btb_mech 0  --fdip_enable 1 --fdip_max_runahead 64 --fdip_break_icache 1 --fdip_nlp 1 --perfect_icache 1 &> log.txt & #
  cd ..;
  
  mkdir perfect_bp;
  cd perfect_bp;
  cp $params .;
  $sim_binary --frontend pt --cbp_trace_r0=/mnt/storage/takh/pgp/workloads/$i/trace.gz --fetch_off_path_ops=false --inst_limit 99999990 --btb_entries 8192 --btb_assoc 4 --enable_crs 1 --crs_entries 32 --enable_ibp 1 --crs_realistic 1 --use_pat_hist 1 --bp_mech tagescl --btb_mech 0  --fdip_enable 1 --fdip_max_runahead 64 --fdip_break_icache 1 --fdip_nlp 1 --perfect_bp 1 &> log.txt & # --perfect_ibp 1 --perfect_crs 1
  cd ..;
  
  mkdir perfect_btb;
  cd perfect_btb;
  cp $params .;
  $sim_binary --frontend pt --cbp_trace_r0=/mnt/storage/takh/pgp/workloads/$i/trace.gz --fetch_off_path_ops=false --inst_limit 99999990 --btb_entries 8192 --btb_assoc 4 --enable_crs 1 --crs_entries 32 --enable_ibp 1 --crs_realistic 1 --use_pat_hist 1 --bp_mech tagescl --btb_mech 0  --fdip_enable 1 --fdip_max_runahead 64 --fdip_break_icache 1 --fdip_nlp 1 --perfect_btb 1 &> log.txt & #
  cd ..;

  cd ..;
  # wait;
done
cd ..;
wait;

cd limit-study;
for i in *; do echo -n "$i ";for j in baseline perfect_icache perfect_bp perfect_btb; do grep IPC $i/$j/bp.stat.0.out|awk '{printf "%f ",$7}';done; echo "";done|awk 'BEGIN{print "Application Ideal-I-cache Ideal-BTB"}{print $1,100*(($3/$2)-1),100*(($5/$2)-1)}' > limit-study.txt;