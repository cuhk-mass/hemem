set terminal pdf
set output 'sizesweep.pdf'
set ylabel 'Time [s]'
set xlabel 'Hotset size [GB]'
set logscale x
set yrange [0:]

plot 'sizesweep.mmgr_simple_mmm.txt' u ($1 / 1024 / 1024 / 1024):($3 / 1000) w linespoints title 'Memory mode', \
     'sizesweep.mmgr_hemem.txt' u ($1 / 1024 / 1024 / 1024):($3 / 1000) w linespoints title 'HeMem'
