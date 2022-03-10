sim_binary="/mnt/storage/michaezh/scarab_hlitz/src/bin/Linux/opt/scarab";
params="/mnt/storage/michaezh/scarab_hlitz/test/PARAMS.in";
files="/mnt/storage/michaezh/scarab_hlitz/test/storage.txt";

rm -rf swipe;
mkdir swipe;
cd swipe;
COUNTER=0;
while IFS="" read -r i || [ -n "$i" ]
do
  COUNTER=$((COUNTER+1));	
  mkdir $COUNTER;
  cd $COUNTER;
  cp $params .;
  if (( COUNTER > 0 )); then
    $sim_binary --frontend pt --cbp_trace_r0=$i --fetch_off_path_ops=false --inst_limit 100000000 --btb_entries 8192 --btb_assoc 4 --enable_crs 1 --crs_entries 32 --enable_ibp 1 --crs_realistic 1 --use_pat_hist 1 --bp_mech tagescl --btb_mech 0 --fdip_enable 1 --fdip_max_runahead 64 --fdip_break_icache 1 --fdip_nlp 1 &> log.txt &
  fi
    if ((COUNTER%30==0));
    then
	echo "waiting $COUNTER";
	wait;
    fi
  cd ..;
  # ../src/scarab --frontend pt --cbp_trace_r0=/mnt/storage/takh/pgp/workloads/$i/trace.gz --fetch_off_path_ops=false --inst_limit 49999995 --btb_entries 8192 --btb_assoc 4 --enable_crs 1 --crs_entries 32 --crs_realistic 1 --enable_ibp 1 --use_pat_hist 1 --bp_mech tagescl --btb_mech 2 --warmup 49999995 &> pgo_btb/$i/$i.txt; mv *.out pgo_btb/$i/;
done < $files
cd ..;
wait;
