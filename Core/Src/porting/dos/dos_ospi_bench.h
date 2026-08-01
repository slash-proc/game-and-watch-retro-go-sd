/* See dos_ospi_bench.c. Measurement build only; DOS_OSPI_BENCH=1 to enable. */
#ifndef DOS_OSPI_BENCH_H
#define DOS_OSPI_BENCH_H

#ifndef DOS_OSPI_BENCH
#define DOS_OSPI_BENCH 0
#endif

#if DOS_OSPI_BENCH
void dos_ospi_bench(void);
#else
#define dos_ospi_bench() do { } while (0)
#endif

#endif /* DOS_OSPI_BENCH_H */
