/********************************************************************
 *  Cache size detection for ARM, PA‑RISC, SPARC64, PowerPC,
 *  x86/x86‑64 and many other POSIX‑like systems.
 *
 *  Compile:
 *      gcc -O3 -march=native -o cacheinfo cacheinfo.c
 *
 *  Run:
 *      ./cacheinfo
 *
 *  Results:
 *      * On x86/x86‑64 the program prints the exact L1/L2/L3 sizes
 *        read from CPUID (leaf 4).
 *      * On all other CPUs it measures the latency for an array of
 *        varying size and reports the first size that causes the
 *        latency to increase noticeably.  That size is the best
 *        estimate of the corresponding cache level.
 ********************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>

/* ------------------------------------------------------------------
 *  Utility: read a high‑resolution counter
 *  - On x86 we use RDTSC
 *  - On PowerPC we use mftb (time base)
 *  - On SPARC we use rd %tbr (time‑base register) if available
 *  - On PA‑RISC we use rdtsc (if the instruction exists)
 *  - Otherwise we fall back to clock_gettime(CLOCK_MONOTONIC)
 * ------------------------------------------------------------------ */
static inline uint64_t get_counter(void)
{
#if defined(__x86_64__) || defined(__i386__)
    unsigned int hi, lo;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)lo) | (((uint64_t)hi) << 32);
#elif defined(__powerpc64__) || defined(__ppc__) || defined(__PPC__)
    /* The time‑base increments once per cycle on most PowerPCs */
    unsigned int hi, lo;
    __asm__ volatile ("mftb %0\nmftb %1\n" : "=r"(hi), "=r"(lo));
    return ((uint64_t)hi << 32) | lo;
#elif defined(__sparc__) || defined(__sparc64__)
    /* The time base register %tbr is available on many SPARCs */
    unsigned long tbr;
    __asm__ volatile ("rd %%tbr, %0" : "=r"(tbr));
    return (uint64_t)tbr;
#elif defined(__hppa__)
    /* PA‑RISC has a rdtsc instruction (not widely supported) */
    unsigned long ts;
    __asm__ volatile ("rdtsc" : "=r"(ts));
    return ts;
#else
    /* Generic POSIX fallback */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
#endif
}

/* ------------------------------------------------------------------
 *  Timing helper: return the elapsed number of counter ticks
 * ------------------------------------------------------------------ */
static inline uint64_t elapsed_ticks(uint64_t start, uint64_t end)
{
    return end - start;
}

/* ------------------------------------------------------------------
 *  Helper: print a message with the cache size in KiB
 * ------------------------------------------------------------------ */
static void print_cache_kb(const char *label, size_t bytes)
{
    printf("%-10s %6zu KiB\n", label, bytes / 1024);
}

/* ------------------------------------------------------------------
 *  1.  CPUID based detection (x86/x86‑64 only)
 * ------------------------------------------------------------------ */
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>

static void cpuid_cache_info(void)
{
    unsigned int eax, ebx, ecx, edx;
    unsigned int cache_type, cache_level, cache_self_initializing,
                 cache_fully_associative, line_size, partitions,
                 ways, sets, cache_size_kb;
    unsigned int i = 0;

    printf("\nCPUID cache information (leaf 4, sub‑leaf %u…):\n",
           i);

    while (1)
    {
        /* __cpuid_count is a GCC/Clang builtin that wraps CPUID */
        __cpuid_count(4, i, eax, ebx, ecx, edx);

        cache_type = eax & 0x1F;
        if (cache_type == 0)                /* No more cache entries */
            break;

        cache_level = (eax >> 5) & 0x7;
        line_size   = (ebx & 0xFFF) + 1;
        partitions  = ((ebx >> 12) & 0x3FF) + 1;
        ways        = ((ebx >> 22) & 0x3FF) + 1;
        sets        = ecx + 1;

        cache_size_kb = (ways * partitions * line_size * sets) >> 10;

        printf("    Level %u: %4u KiB, %4u ways, line size %3u bytes, "
               "%5u sets\n",
               cache_level, cache_size_kb, ways, line_size, sets);

        i++;
    }
}
#endif /* CPUID */

/* ------------------------------------------------------------------
 *  2.  Timing based detection
 *
 *  The idea:
 *      • Allocate a large array (at least a few MB).
 *      • Touch every element in a loop while varying the array size.
 *      • Measure the average latency per element.
 *      • When the latency jumps, the array size has overflowed the
 *        current cache level.
 *
 *  The thresholds are empirical – we look for a >20 % increase between
 *  successive sizes.  The array stride is 64 bytes to stay on cache
 *  line boundaries (the common line size on all modern CPUs).
 * ------------------------------------------------------------------ */
static const size_t CACHE_LINE = 64;          /* 64‑byte line size is safe */
static const size_t MAX_ARRAY   = 64 * 1024 * 1024; /* 64 MiB maximum */
static const size_t ITERATIONS  = 2000;       /* Number of measurement loops */

static size_t measure_access_latency(size_t array_size)
{
    /* Allocate the array once and keep it alive across calls */
    static int *buf = NULL;
    static size_t allocated = 0;

    if (!buf || allocated < array_size)
    {
        free(buf);
        buf = (int *)aligned_alloc(CACHE_LINE, array_size);
        if (!buf) { perror("aligned_alloc"); exit(1); }
        allocated = array_size;
    }

    /* Warm the cache to avoid first‑touch effects */
    for (size_t i = 0; i < array_size; i += CACHE_LINE)
        buf[i / sizeof(int)] = 0;

    volatile long long acc = 0;
    uint64_t start = get_counter();

    for (size_t iter = 0; iter < ITERATIONS; ++iter)
        for (size_t i = 0; i < array_size; i += CACHE_LINE)
            acc += buf[i / sizeof(int)] ^ i;

    uint64_t end = get_counter();
    (void)acc;  /* silence unused warning */

    /* Return average ticks per element (not per cache line) */
    return (end - start) / (array_size / sizeof(int) * ITERATIONS);
}

/* Detect cache sizes by searching for a jump in latency.  */
static void timing_cache_detection(void)
{
    printf("\nTiming‑based cache size detection:\n");

    size_t prev_size = 0;
    uint64_t prev_lat = 0;
    size_t current_size = CACHE_LINE;   /* start with one line */
    int level = 1;

    while (current_size <= MAX_ARRAY)
    {
        uint64_t latency = measure_access_latency(current_size);

        /* If this is the first measurement, just record it */
        if (prev_size == 0)
        {
            prev_size = current_size;
            prev_lat  = latency;
            current_size <<= 1;      /* double size */
            continue;
        }

        /* Look for a big jump in latency (>20 %) */
        if (latency > prev_lat * 120 / 100)
        {
            print_cache_kb("Cache level", prev_size);
            level++;
            prev_size = 0;          /* reset for the next level */
        }
        else
        {
            prev_size = current_size;
            prev_lat  = latency;
        }

        current_size <<= 1;
    }

    /* If we never saw a jump, we did not hit a cache boundary – report the largest array */
    if (prev_size && prev_size < MAX_ARRAY)
        print_cache_kb("Cache level", prev_size);
}

/* ------------------------------------------------------------------
 *  3.  Main entry point
 * ------------------------------------------------------------------ */
int main(void)
{
    printf("Cache size detection – %s (%s)\n\n",
           __DATE__, __TIME__);

#if defined(__x86_64__) || defined(__i386__)
    cpuid_cache_info();
    timing_cache_detection();  /* also run timing fallback for sanity */
#else
    timing_cache_detection();
#endif

    return 0;
}
