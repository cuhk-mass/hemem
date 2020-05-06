#!/bin/sh

for sizepower in `seq 20 37`; do
    size=$((1 << sizepower))
    awk "NR == 2 || NR == 4 { print $size, \$1, \$2 }" sizesweep.$size.txt
done | sort -k2,2 > sizesweep.txt

awk '$2 == "./mmgr_simple_mmm" { print }' sizesweep.txt | sort -n -k1,1 > sizesweep.mmgr_simple_mmm.txt
awk '$2 == "./mmgr_hemem" { print }' sizesweep.txt | sort -n -k1,1 > sizesweep.mmgr_hemem.txt
