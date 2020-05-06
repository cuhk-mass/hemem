#!/bin/sh

# Hotset size sweep
for sizepower in `seq 20 37`; do
    size=$((1 << sizepower))
    echo Size sweep. Size $size bytes.
    make -B bench HOTSET_SIZE=$size
    mv results.txt sizesweep.$size.txt
done
