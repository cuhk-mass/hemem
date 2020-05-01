set terminal pdf
set output 'evalsweep.pdf'
set ylabel 'Time [us]'
set xlabel 'Memory size [GB]'
set logscale x

plot 'evalsweep.0.txt' u ($1 / 1024 / 1024 / 1024):3 w linespoints title 'GIGA', \
     'evalsweep.1.txt' u ($1 / 1024 / 1024 / 1024):3 w linespoints title 'HUGE', \
     'evalsweep.2.txt' u ($1 / 1024 / 1024 / 1024):3 w linespoints title 'BASE'
