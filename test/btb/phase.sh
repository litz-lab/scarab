declare -a benchmarks=("cassandra" "drupal" "finagle-chirper" "finagle-http" "kafka" "mediawiki" "tomcat" "verilator" "wordpress");

for b in "${benchmarks[@]}";
do
  rm -rf $b;
  mkdir $b;
  zcat $b.gz|awk '{current_bin=int($3/1000000);brcount[current_bin]++;if($7=="1"){misscount[current_bin]++;}uniq_branch[current_bin][$5]++;uniq_target[current_bin][$6]++;uniq_entry[current_bin][$5" "$6]++;}END{for(current_bin in brcount){c1=0;c2=0;c3=0;for(j in uniq_branch[current_bin]){c1++;}for(j in uniq_target[current_bin]){c2++;}for(j in uniq_entry[current_bin]){c3++;}m=0;if(current_bin in misscount)m=misscount[current_bin];print current_bin,(100.0*m/brcount[current_bin]),brcount[current_bin],m,c1,c2,c3;}}' > $b/phases.txt;
  # zcat $b.gz|awk '{aa[$5]++;ab[$6]++;ac[$5][$6]++;ad[$6][$5]++;if($7=="1"){ma[$5]++;mb[$6]++;mc[$5][$6]++;md[$6][$5]++;}}END{for(pc in aa){c=0;for(target in ac[pc]){c++;}print pc,aa[pc],c;}}'
  # awk '{a[$4][$5][$6]++}END{for(type in a){total=0;for(pc in a[type]){c1=0;c2=0;for(target in a[type][pc]){c1+=1;c2+=a[type][pc][target]}static[c1]+=1;dynamic[c1]+=c2;total+=c2;}printf "%s %d ", type, total;for(c in static){printf "%d->%d,%d ",c,static[c],dynamic[c]}print "";delete static;delete dynamic;}}'
done