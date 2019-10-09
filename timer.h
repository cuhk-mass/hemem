#ifndef HEMEM_TIMER_H
#define HEMEM_TIMER_H

/* Returns the number of seconds encoded in T, a "struct timeval". */
#define tv_to_double(t) (t.tv_sec + (t.tv_usec / 1000000.0))

void timeDiff(struct timeval *d, struct timeval *a, struct timeval *b);
double elapsed(struct timeval *starttime, struct timeval *endtime);

#endif /* HEMEM_TIMER_H */
