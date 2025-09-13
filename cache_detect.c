#if !defined(_WIN32)
#  if !defined(_POSIX_C_SOURCE)
#    define _POSIX_C_SOURCE 200809L
#  endif
#endif

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

// Prevent elimination by optimizer
static volatile void *volatile g_sink;

// Portable high-resolution timer returning nanoseconds
static uint64_t now_ns(void) {
#if defined(__APPLE__)
	static mach_timebase_info_data_t timebase = {0};
	if (timebase.denom == 0) {
		mach_timebase_info(&timebase);
	}
	uint64_t t = mach_absolute_time();
	return (t * (uint64_t)timebase.numer) / (uint64_t)timebase.denom;
#elif defined(CLOCK_MONOTONIC_RAW)
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#elif defined(CLOCK_MONOTONIC)
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#else
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

// Simple xorshift64 RNG for reproducible shuffles
typedef struct Random64 {
	uint64_t state;
} Random64;

static inline uint64_t rotl64(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

static inline uint64_t rng_next(Random64 *r) {
	// xorshift* (xoroshiro-like) variant
	uint64_t x = r->state;
	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	r->state = x;
	return x * 2685821657736338717ULL;
}

static inline size_t rng_uniform(Random64 *r, size_t n) {
	// unbiased range [0, n)
	uint64_t threshold = (~(uint64_t)0) % n;
	for (;;) {
		uint64_t x = rng_next(r);
		if (x >= threshold) return (size_t)(x % n);
	}
}

// Build one Hamiltonian cycle over nodes spaced by node_stride within base buffer.
// Each node stores the pointer to the next node at offset 0.
static void build_cycle(uint8_t *base, size_t num_nodes, size_t node_stride, size_t *perm, Random64 *rng) {
	for (size_t i = 0; i < num_nodes; ++i) perm[i] = i;
	// Fisher–Yates shuffle
	for (size_t i = num_nodes - 1; i > 0; --i) {
		size_t j = rng_uniform(rng, i + 1);
		size_t tmp = perm[i];
		perm[i] = perm[j];
		perm[j] = tmp;
	}
	for (size_t i = 0; i < num_nodes; ++i) {
		size_t from = perm[i];
		size_t to = perm[(i + 1) % num_nodes];
		uint8_t *from_ptr = base + from * node_stride;
		uint8_t *to_ptr = base + to * node_stride;
		// store the next pointer at start of node
		// alignment is ensured by posix_memalign
		*(void **)from_ptr = (void *)to_ptr;
	}
}

// Build using a specific order array

typedef enum Pattern {
	PATTERN_RANDOM = 0,
	PATTERN_SEQUENTIAL,
	PATTERN_REVERSE,
	PATTERN_STRIDE,
	PATTERN_INTERLEAVE,
	PATTERN_GRAY,
	PATTERN_BITREVERSE
} Pattern;

// Build using a specific order array
static void build_cycle_from_order(uint8_t *base, size_t num_nodes, size_t node_stride, const size_t *order) {
	for (size_t i = 0; i < num_nodes; ++i) {
		size_t from = order[i];
		size_t to = order[(i + 1) % num_nodes];
		uint8_t *from_ptr = base + from * node_stride;
		uint8_t *to_ptr = base + to * node_stride;
		*(void **)from_ptr = (void *)to_ptr;
	}
}

static void build_order_random(size_t *order, size_t num_nodes, Random64 *rng) {
	for (size_t i = 0; i < num_nodes; ++i) order[i] = i;
	for (size_t i = num_nodes - 1; i > 0; --i) {
		size_t j = rng_uniform(rng, i + 1);
		size_t tmp = order[i];
		order[i] = order[j];
		order[j] = tmp;
	}
}

static void build_order_sequential(size_t *order, size_t num_nodes) {
	for (size_t i = 0; i < num_nodes; ++i) order[i] = i;
}

static void build_order_reverse(size_t *order, size_t num_nodes) {
	for (size_t i = 0; i < num_nodes; ++i) order[i] = num_nodes - 1 - i;
}

static void build_order_stride(size_t *order, size_t num_nodes, size_t k) {
	if (num_nodes == 0) return;
	if (k == 0) k = 1;
	uint8_t *visited = (uint8_t *)calloc(num_nodes, 1);
	if (!visited) {
		build_order_sequential(order, num_nodes);
		return;
	}
	size_t count = 0;
	size_t start = 0;
	while (count < num_nodes) {
		size_t i = start;
		while (!visited[i]) {
			order[count++] = i;
			visited[i] = 1;
			i = (i + k) % num_nodes;
		}
		if (count < num_nodes) {
			while (start < num_nodes && visited[start]) start++;
			if (start >= num_nodes) break;
		}
	}
	free(visited);
}

static void build_order_interleave(size_t *order, size_t num_nodes) {
	size_t half = num_nodes / 2;
	size_t out = 0;
	for (size_t i = 0; i < half; ++i) {
		order[out++] = i;
		order[out++] = i + half;
	}
	if ((num_nodes & 1u) != 0) {
		order[out++] = num_nodes - 1;
	}
}

static void build_order_gray(size_t *order, size_t num_nodes) {
	if (num_nodes == 0) return;
	size_t m = 1;
	while ((m << 1) > m && (m << 1) <= num_nodes) m <<= 1;
	size_t out = 0;
	for (size_t i = 0; i < m; ++i) {
		order[out++] = (i ^ (i >> 1));
	}
	for (size_t i = m; i < num_nodes; ++i) {
		order[out++] = i;
	}
}

static inline size_t reverse_bits_limited(size_t x, unsigned bits) {
	size_t r = 0;
	for (unsigned i = 0; i < bits; ++i) {
		r = (r << 1) | ((x >> i) & 1u);
	}
	return r;
}

static void build_order_bitrev(size_t *order, size_t num_nodes) {
	if (num_nodes == 0) return;
	unsigned bits = 0;
	size_t tmp = num_nodes - 1;
	while (tmp > 0) { bits++; tmp >>= 1; }
	size_t out = 0;
	size_t limit = ((size_t)1) << bits;
	for (size_t i = 0; i < limit; ++i) {
		size_t rev = reverse_bits_limited(i, bits);
		if (rev < num_nodes) order[out++] = rev;
		if (out == num_nodes) break;
	}
}

static void build_cycle_pattern(uint8_t *base, size_t num_nodes, size_t node_stride, size_t *order, Random64 *rng, Pattern p, size_t pattern_arg) {
	switch (p) {
		case PATTERN_RANDOM:      build_order_random(order, num_nodes, rng); break;
		case PATTERN_SEQUENTIAL:  build_order_sequential(order, num_nodes); break;
		case PATTERN_REVERSE:     build_order_reverse(order, num_nodes); break;
		case PATTERN_STRIDE:      build_order_stride(order, num_nodes, pattern_arg == 0 ? 1 : pattern_arg); break;
		case PATTERN_INTERLEAVE:  build_order_interleave(order, num_nodes); break;
		case PATTERN_GRAY:        build_order_gray(order, num_nodes); break;
		case PATTERN_BITREVERSE:  build_order_bitrev(order, num_nodes); break;
		default:                  build_order_random(order, num_nodes, rng); break;
	}
	build_cycle_from_order(base, num_nodes, node_stride, order);
}

// Pointer-chase for given number of steps starting at head
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static void *chase(void *head, size_t steps) {
	void *p = head;
	for (size_t i = 0; i < steps; ++i) {
		p = *(void * volatile *)p; // force actual memory load
	}
	g_sink = p; // observable side-effect
	return p;
}

typedef struct Sample {
	size_t working_set_bytes;
	double ns_per_access;
} Sample;

typedef struct Options {
	size_t min_bytes;
	size_t max_bytes;
	size_t node_stride;
	unsigned warmup_iters;
	unsigned target_ms;
	unsigned repeats;
	bool print_table;
	Pattern pattern;
	size_t pattern_arg; // e.g. stride step for PATTERN_STRIDE
} Options;

static size_t clamp_size(size_t v, size_t lo, size_t hi) {
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

static const char *pattern_name(Pattern p) {
	switch (p) {
		case PATTERN_RANDOM: return "random";
		case PATTERN_SEQUENTIAL: return "seq";
		case PATTERN_REVERSE: return "reverse";
		case PATTERN_STRIDE: return "stride";
		case PATTERN_INTERLEAVE: return "interleave";
		case PATTERN_GRAY: return "gray";
		case PATTERN_BITREVERSE: return "bitrev";
		default: return "random";
	}
}

static Pattern parse_pattern(const char *s) {
	if (strcmp(s, "random") == 0) return PATTERN_RANDOM;
	if (strcmp(s, "seq") == 0 || strcmp(s, "sequential") == 0) return PATTERN_SEQUENTIAL;
	if (strcmp(s, "reverse") == 0) return PATTERN_REVERSE;
	if (strcmp(s, "stride") == 0) return PATTERN_STRIDE;
	if (strcmp(s, "interleave") == 0) return PATTERN_INTERLEAVE;
	if (strcmp(s, "gray") == 0 || strcmp(s, "graycode") == 0) return PATTERN_GRAY;
	if (strcmp(s, "bitrev") == 0 || strcmp(s, "bitreverse") == 0) return PATTERN_BITREVERSE;
	return PATTERN_RANDOM;
}

static void parse_args(int argc, char **argv, Options *opt) {
	// defaults chosen for portability across 32/64-bit
	opt->min_bytes = 4 * 1024;
	opt->max_bytes = 256 * 1024 * 1024ull;
	opt->node_stride = 256; // ensure > typical cache line on all targets
	opt->warmup_iters = 3;
	opt->target_ms = 80;    // aim ~80ms per sample
	opt->repeats = 3;       // average of repeats
	opt->print_table = true;
	opt->pattern = PATTERN_RANDOM;
	opt->pattern_arg = 1;
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--min-bytes") == 0 && i + 1 < argc) {
			unsigned long long v = strtoull(argv[++i], NULL, 0);
			if (v > (unsigned long long)SIZE_MAX) v = (unsigned long long)SIZE_MAX; // avoid 32-bit wrap
			opt->min_bytes = (size_t)v;
		} else if (strcmp(argv[i], "--max-bytes") == 0 && i + 1 < argc) {
			unsigned long long v = strtoull(argv[++i], NULL, 0);
			if (v > (unsigned long long)SIZE_MAX) v = (unsigned long long)SIZE_MAX; // avoid 32-bit wrap
			opt->max_bytes = (size_t)v;
		} else if (strcmp(argv[i], "--node-stride") == 0 && i + 1 < argc) {
			opt->node_stride = (size_t)strtoull(argv[++i], NULL, 0);
		} else if (strcmp(argv[i], "--target-ms") == 0 && i + 1 < argc) {
			opt->target_ms = (unsigned)strtoul(argv[++i], NULL, 0);
		} else if (strcmp(argv[i], "--repeats") == 0 && i + 1 < argc) {
			opt->repeats = (unsigned)strtoul(argv[++i], NULL, 0);
		} else if ((strcmp(argv[i], "--pattern") == 0 || strcmp(argv[i], "-p") == 0) && i + 1 < argc) {
			opt->pattern = parse_pattern(argv[++i]);
		} else if (strcmp(argv[i], "--pattern-arg") == 0 && i + 1 < argc) {
			opt->pattern_arg = (size_t)strtoull(argv[++i], NULL, 0);
		} else if (strcmp(argv[i], "--no-table") == 0) {
			opt->print_table = false;
		} else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			printf("Usage: %s [--min-bytes N] [--max-bytes N] [--node-stride N] [--target-ms N] [--repeats N] [--pattern NAME] [--pattern-arg N] [--no-table]\n", argv[0]);
			printf("  Patterns: random (default), seq, reverse, stride, interleave, gray, bitrev\n");
			printf("  For stride pattern, use --pattern-arg K to set step (default 1).\n");
			exit(0);
		}
	}
	// sanity bounds
	opt->min_bytes = clamp_size(opt->min_bytes, opt->node_stride * 2, opt->max_bytes);
	// Clamp upper bound to 4 GiB, but cap at SIZE_MAX to avoid 32-bit wrap
	uint64_t hi64 = 4ull * 1024 * 1024 * 1024;
	size_t hi = (hi64 > (uint64_t)SIZE_MAX) ? SIZE_MAX : (size_t)hi64;
	opt->max_bytes = clamp_size(opt->max_bytes, opt->min_bytes, hi);
}

static int cmp_size_t(const void *a, const void *b) {
	const size_t *aa = (const size_t *)a;
	const size_t *bb = (const size_t *)b;
	if (*aa < *bb) return -1;
	if (*aa > *bb) return 1;
	return 0;
}

// Generate working-set sizes: for each power-of-two, also test 1.5x that size
static size_t generate_sizes(size_t min_bytes, size_t max_bytes, size_t *out, size_t out_cap) {
	// ensure min_bytes is a multiple of 2*node_stride to have at least 2 nodes
	(void)cmp_size_t; // silence unused if not used by some compilers
	size_t count = 0;
	// find highest power-of-two <= min_bytes without builtins
	size_t p = 1;
	while ((p << 1) > p && (p << 1) <= min_bytes) {
		p <<= 1;
	}
	if (p < 1024) p = 1024;
	for (; p <= max_bytes; p <<= 1) {
		if (p >= min_bytes) {
			if (count < out_cap) out[count++] = p;
		}
		// Always add 1.5x
		size_t v15 = p + p / 2;
		if (v15 >= min_bytes && v15 <= max_bytes) {
			if (count < out_cap) out[count++] = v15;
		}
		// For smaller sizes, add denser steps
		if (p <= (1u << 20)) { // <= 1 MiB
			size_t v125 = p + p / 4; // 1.25x
			size_t v175 = p + (p * 3) / 4; // 1.75x
			if (v125 >= min_bytes && v125 <= max_bytes) {
				if (count < out_cap) out[count++] = v125;
			}
			if (v175 >= min_bytes && v175 <= max_bytes) {
				if (count < out_cap) out[count++] = v175;
			}
		}
		if (p <= (128u << 10)) { // <= 128 KiB
			size_t v1125 = p + p / 8; // 1.125x
			size_t v1375 = p + (p * 3) / 8; // 1.375x
			size_t v1625 = p + (p * 5) / 8; // 1.625x
			size_t v1875 = p + (p * 7) / 8; // 1.875x
			if (v1125 >= min_bytes && v1125 <= max_bytes) {
				if (count < out_cap) out[count++] = v1125;
			}
			if (v1375 >= min_bytes && v1375 <= max_bytes) {
				if (count < out_cap) out[count++] = v1375;
			}
			if (v1625 >= min_bytes && v1625 <= max_bytes) {
				if (count < out_cap) out[count++] = v1625;
			}
			if (v1875 >= min_bytes && v1875 <= max_bytes) {
				if (count < out_cap) out[count++] = v1875;
			}
		}
		if (p > (SIZE_MAX >> 1)) break;
	}
	// sort to ensure ascending order
	qsort(out, count, sizeof(out[0]), cmp_size_t);
	return count;
}

// Measure ns per pointer-chase access for a given working set size
static double measure_ns_per_access(uint8_t *base, size_t working_set_bytes, size_t node_stride, size_t *perm, Random64 *rng, const Options *opt) {
	// number of nodes
	size_t nodes = working_set_bytes / node_stride;
	if (nodes < 2) nodes = 2; // minimal cycle
	build_cycle_pattern(base, nodes, node_stride, perm, rng, opt->pattern, opt->pattern_arg);
	void *head = (void *)base;
	// warmup
	for (unsigned w = 0; w < opt->warmup_iters; ++w) {
		(void)chase(head, nodes);
	}
	// adaptive run length to hit ~target_ms
	uint64_t target_ns = (uint64_t)opt->target_ms * 1000000ull;
	// Start with a few passes
	uint64_t steps = nodes * 16ull;
	if (steps < 1000ull) steps = 1000ull;
	double best_ns_per = 1e300;
	for (unsigned r = 0; r < opt->repeats; ++r) {
		// adapt steps
		for (;;) {
			atomic_signal_fence(memory_order_seq_cst);
			uint64_t t0 = now_ns();
			(void)chase(head, (size_t)steps);
			uint64_t t1 = now_ns();
			atomic_signal_fence(memory_order_seq_cst);
			uint64_t dt = t1 - t0;
			if (dt >= target_ns / 2 || steps > (1ull << 62)) break;
			steps *= 2;
		}
		atomic_signal_fence(memory_order_seq_cst);
		uint64_t t0 = now_ns();
		(void)chase(head, (size_t)steps);
		uint64_t t1 = now_ns();
		atomic_signal_fence(memory_order_seq_cst);
		uint64_t dt = t1 - t0;
		double ns_per = (double)dt / (double)steps;
		if (ns_per < best_ns_per) best_ns_per = ns_per; // take best of repeats to reduce noise
	}
	return best_ns_per;
}

// Heuristic: detect boundaries where latency jumps vs previous plateau
typedef struct Boundary {
	size_t approx_size_bytes;
	double ratio;
} Boundary;

static size_t detect_boundaries(const Sample *samples, size_t n, Boundary *out, size_t out_cap) {
	if (n == 0) return 0;
	double plateau_sum = samples[0].ns_per_access;
	int plateau_count = 1;
	double plateau_avg = plateau_sum / plateau_count;
	const double jump_threshold = 1.25; // 25% jump
	const int min_plateau_points = 2;
	size_t last_boundary_idx = 0;
	size_t found = 0;
	for (size_t i = 1; i < n; ++i) {
		double ratio = samples[i].ns_per_access / plateau_avg;
		bool sustained = false;
		if (ratio > jump_threshold && (int)(i - last_boundary_idx) >= min_plateau_points) {
			// confirm with a lookahead when possible
			if (i + 1 < n) {
				double ratio_next = samples[i + 1].ns_per_access / plateau_avg;
				sustained = ratio_next > (jump_threshold * 0.95);
			} else {
				sustained = true;
			}
		}
		if (sustained) {
			if (found < out_cap) {
				out[found].approx_size_bytes = samples[i - 1].working_set_bytes;
				out[found].ratio = ratio;
			}
			found++;
			// reset plateau after boundary
			last_boundary_idx = i;
			plateau_sum = samples[i].ns_per_access;
			plateau_count = 1;
			plateau_avg = plateau_sum / plateau_count;
		} else {
			plateau_sum += samples[i].ns_per_access;
			plateau_count++;
			plateau_avg = plateau_sum / plateau_count;
		}
	}
	return found;
}

static const char *human_size(size_t bytes, char *buf, size_t buf_sz) {
	const char *units[] = {"B", "KiB", "MiB", "GiB"};
	int u = 0;
	double v = (double)bytes;
	while (v >= 1024.0 && u < 3) { v /= 1024.0; u++; }
	snprintf(buf, buf_sz, "%.1f %s", v, units[u]);
	return buf;
}

int main(int argc, char **argv) {
	Options opt;
	parse_args(argc, argv, &opt);
	// Generate sizes first
	const size_t max_samples = 1024;
	size_t sizes[max_samples];
	size_t num_sizes = generate_sizes(opt.min_bytes, opt.max_bytes, sizes, max_samples);
	if (num_sizes == 0) {
		fprintf(stderr, "No sizes to test.\n");
		return 1;
	}

	// Try to allocate a buffer large enough for the largest requested size.
	// If allocation fails, progressively reduce to the next smaller size.
	void *raw = NULL;
	size_t alloc_idx = num_sizes - 1;
	size_t alloc_bytes = sizes[alloc_idx];
	int err = posix_memalign(&raw, opt.node_stride, alloc_bytes);
	while ((err != 0 || raw == NULL) && alloc_idx > 0) {
		fprintf(stderr, "Allocation of %zu bytes failed (%s). Retrying with smaller size...\n", alloc_bytes, err ? strerror(err) : "unknown");
		alloc_idx--;
		alloc_bytes = sizes[alloc_idx];
		raw = NULL;
		err = posix_memalign(&raw, opt.node_stride, alloc_bytes);
	}
	if (err != 0 || raw == NULL) {
		fprintf(stderr, "Allocation failed even for smallest size (%zu bytes): %s\n", alloc_bytes, err ? strerror(err) : "unknown");
		return 1;
	}
	// Trim test sizes to those that fit in the allocated buffer
	while (num_sizes > 0 && sizes[num_sizes - 1] > alloc_bytes) {
		num_sizes--;
	}
	uint8_t *base = (uint8_t *)raw;
	memset(base, 0, alloc_bytes);

	// Prepare permutation array for up to max nodes within allocated buffer
	size_t max_nodes = alloc_bytes / opt.node_stride;
	size_t *perm = (size_t *)malloc(max_nodes * sizeof(size_t));
	if (!perm) {
		fprintf(stderr, "Permutation allocation failed\n");
		free(raw);
		return 1;
	}

	Sample *samples = (Sample *)calloc(num_sizes, sizeof(Sample));
	if (!samples) {
		fprintf(stderr, "Sample allocation failed\n");
		free(perm);
		free(raw);
		return 1;
	}

	Random64 rng;
	// seed from address entropy and time
	uint64_t seed = (uint64_t)now_ns() ^ (uint64_t)(uintptr_t)&rng ^ (uint64_t)getpid();
	if (seed == 0) seed = 0x123456789abcdefULL;
	rng.state = seed;

	if (opt.print_table) {
		printf("# Cache size detection via pointer-chasing (node_stride=%zub, pattern=%s", opt.node_stride, pattern_name(opt.pattern));
		if (opt.pattern == PATTERN_STRIDE) {
			printf(", step=%zu", opt.pattern_arg == 0 ? (size_t)1 : opt.pattern_arg);
		}
		printf(")\n");
		printf("# size_bytes\tlatency_ns_per_access\n");
	}

	for (size_t i = 0; i < num_sizes; ++i) {
		size_t ws = sizes[i];
		double ns = measure_ns_per_access(base, ws, opt.node_stride, perm, &rng, &opt);
		samples[i].working_set_bytes = ws;
		samples[i].ns_per_access = ns;
		if (opt.print_table) {
			printf("%zu\t%.3f\n", ws, ns);
			fflush(stdout);
		}
	}

	Boundary bounds[8];
	size_t nb = detect_boundaries(samples, num_sizes, bounds, 8);
	char buf[32];
	printf("\nDetected cache levels (approx):\n");
	for (size_t i = 0; i < nb; ++i) {
		const char *lvl = (i == 0 ? "L1" : (i == 1 ? "L2" : (i == 2 ? "L3" : (i == 3 ? "L4" : "L?"))));
		printf("- %s capacity ~ %s (jump x%.2f)\n", lvl, human_size(bounds[i].approx_size_bytes, buf, sizeof(buf)), bounds[i].ratio);
	}
	if (nb == 0) {
		printf("- No clear cache boundaries detected; try increasing --max-bytes or adjusting --node-stride.\n");
	}

	free(samples);
	free(perm);
	free(raw);
	return 0;
}


