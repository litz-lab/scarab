for i in $(seq 1 105); do
	file="/mnt/storage/michaezh/scarab_hlitz/test/swipe/$i/log.txt"
	while IFS= read -r line; do
		if [[ "$line" == *"Unmapped instruction"* ]]; then
			echo "Unmatched error at $i"
		fi
	done < $file
done
echo "done check"
