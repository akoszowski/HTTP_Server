#ifndef _ERR_
#define _ERR_

/* Wypisuje informację o błędnym zakończeniu funkcji systemowej
i kończy działanie programu. */
void syserr(const char *fmt, ...);

/* Wypisuje informację o błędzie i kończy działanie programu. */
void fatal(const char *fmt, ...);

#endif
