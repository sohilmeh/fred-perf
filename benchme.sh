#!/bin/bash -

declare -a cores=(1 5)
declare -a types=(pcore ecore)
#declare -a cores=(1)
#declare -a types=(pcore)

# Get a sudo cookie ahead of time
sudo cat /dev/null

ver=$(uname -a | awk '{ print $3$4; }' | sed -E -e 's/[][[:space:]$'\''"\\#=!<>|;{}()*?~&]/_/g')
if ./checkfred > /dev/null; then
    sys=fred
else
    sys=idt
fi

count=16
cfmt="%0$(printf '%d' $count | wc -c)d"
declare -a files

uid=$(id -u)
gid=$(id -g)

for ((t = 0; t < ${#types[@]}; t++)); do
    core="${cores[$t]}"
    type="${types[$t]}"
    od="results/$ver/$type/$sys"
    mkdir -p "$od"

    files=()

    for ((i = 1; i <= count; i++)); do
	of="$od"/$(printf $cfmt $i)
	echo "$of"
	if false; then
	    sudo perf record \
		 -C $core -F max \
		 -e frontend_retired.any_dsb_miss:k \
		 -e frontend_retired.dsb_miss:k \
		 -e idq.dsb_cycles_any:k \
		 -e idq.dsb_cycles_ok:k \
		 -e idq.dsb_uops:k \
		 -e frontend_retired.itlb_miss:k \
		 -e icache_data.stalls \
		 -e l1-dcache-load-misses:k \
		 --all-kernel \
		 -o "$of".perf \
		 -- ./fred_bench $core > "$of".csv
	    sudo chown $uid:$gid "$of".perf
	else
	    sudo ./fred_bench $core > "$of".csv
	fi
	files+=("$of".csv)
    done

    (
	printf '"%s","%s","%s"\n' $type $sys $ver
	./analyze.pl "${files[@]}"
    ) > "$od"/stats.csv
done
