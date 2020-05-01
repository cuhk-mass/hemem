#!/bin/sh

echo "# memsize pt us pages_swept[0, 1, 2] allocsize"

for p in `seq 0 2`; do
    for i in `seq 20 48`; do
	MEMSIZE=$((1 << i))

	test $((p == 2 && i > 43)) = 1 && exit 0
	
	echo ./pgsweep $MEMSIZE $p >/dev/stderr
	./pgsweep $MEMSIZE $p
    done >evalsweep.$p.txt
done
