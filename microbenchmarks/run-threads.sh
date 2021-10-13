#clear-caches
echo "=== 1 ===" > threads.txt
numactl -N0 --preferred=0 -- ./gups-hotset-move 1 1000000000 39 8 34 >> threads.txt
#clear-caches
echo "=== 2 ===" >> threads.txt
numactl -N0 --preferred=0 -- ./gups-hotset-move 2 1000000000 39 8 34 >> threads.txt
#clear-caches
echo "=== 4 ===" >> threads.txt
numactl -N0 --preferred=0 -- ./gups-hotset-move 4 1000000000 39 8 34 >> threads.txt
#clear-caches
echo "=== 8 ===" >> threads.txt
numactl -N0 --preferred=0 -- ./gups-hotset-move 8 1000000000 39 8 34 >> threads.txt
#clear-caches
echo "=== 16 ===" >> threads.txt
numactl -N0 --preferred=0 -- ./gups-hotset-move 16 1000000000 39 8 34 >> threads.txt
#clear-caches
echo "=== 20 ===" >> threads.txt
numactl -N0 --preferred=0 -- ./gups-hotset-move 16 1000000000 39 8 34 >> threads.txt
#clear-caches
echo "=== 21 ===" >> threads.txt
numactl -N0 --preferred=0 -- ./gups-hotset-move 16 1000000000 39 8 34 >> threads.txt
#clear-caches
echo "=== 22 ===" >> threads.txt
numactl -N0 --preferred=0 -- ./gups-hotset-move 16 1000000000 39 8 34 >> threads.txt
#clear-caches
echo "=== 23 ===" >> threads.txt
numactl -N0 --preferred=0 -- ./gups-hotset-move 16 1000000000 39 8 34 >> threads.txt
#clear-caches
echo "=== 24 ===" >> threads.txt
numactl -N0 --preferred=0 -- ./gups-hotset-move 16 1000000000 39 8 34 >> threads.txt
#clear-caches
