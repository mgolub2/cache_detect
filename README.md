### Cache Detect

This repository contains a portable cache latency microbenchmark and helper tooling to run it across hosts and visualize results.

- **`cache_detect.c`**: Pointer-chasing benchmark to detect cache latencies and approximate cache sizes.
- **`sync_build_run.py`**: Syncs sources to remote hosts via SSH, builds once, runs the benchmark for selected patterns up to 1 GiB, and collects logs locally.
- **`plot_cache_logs.py`**: Plots one or more benchmark logs into a single figure and writes a combined CSV.
- **`Makefile`**: Builds the C benchmark and an auxiliary test target.
- **`4GB/*.txt`**: Example output logs captured on various CPUs for a 4 GiB run.

### Build

Requirements: a C11 compiler and `make`.

```bash
make        # build cache_detect
make run    # run locally with defaults
make clean  # remove build artifacts
```

### `cache_detect` usage

The benchmark performs pointer-chasing across a working set and reports nanoseconds per access vs. working-set size. It also prints an approximate cache level summary.

Flags:
- **`--min-bytes N`**: Minimum working-set size in bytes (default: 4096).
- **`--max-bytes N`**: Maximum working-set size in bytes (default: 256 MiB; script uses larger).
- **`--node-stride N`**: Spacing between nodes in bytes (default: 256).
- **`--target-ms N`**: Target runtime per sample (default: 80 ms).
- **`--repeats N`**: Repeated trials per sample; best taken (default: 3).
- **`--pattern NAME`**: Pointer-chase order pattern (default: `random`).
  - Supported: `random`, `seq`, `reverse`, `stride`, `interleave`, `gray`, `bitrev`.
- **`--pattern-arg N`**: Optional argument for the pattern (used by `stride` as the step; default: 1).
- **`--no-table`**: Suppress printing the data table.
- **`-h`, `--help`**: Show help.

Examples:

```bash
# Default random pattern, from 1 KiB to 1 GiB
./cache_detect --min-bytes 1024 --max-bytes 1073741824

# Sequential pattern
./cache_detect --pattern seq --max-bytes 1073741824

# Reverse pattern
./cache_detect --pattern reverse --max-bytes 1073741824

# Stride by 4 nodes (with node_stride = 256 bytes)
./cache_detect --pattern stride --pattern-arg 4 --node-stride 256 --max-bytes 1073741824

# Interleave halves
./cache_detect --pattern interleave --max-bytes 1073741824

# Gray-code order over nearest power-of-two
./cache_detect --pattern gray --max-bytes 1073741824

# Bit-reversal order
./cache_detect --pattern bitrev --max-bytes 1073741824
```

Output format (table header commented with `#`):

```text
# Cache size detection via pointer-chasing (node_stride=256b, pattern=random)
# size_bytes	latency_ns_per_access
1024	0.80
1536	0.81
...

Detected cache levels (approx):
- L1 capacity ~ 32.0 KiB (jump x1.42)
- L2 capacity ~ 512.0 KiB (jump x1.36)
- L3 capacity ~ 32.0 MiB (jump x1.28)
```

### `sync_build_run.py` (remote sync/build/run)

Synchronizes `Makefile` and `cache_detect.c` to remote hosts over SSH, builds once per host, then runs selected patterns up to 1 GiB and writes each pattern’s output as a separate local file named `CPU Name (pattern).txt`.

Requirements:
- Passwordless SSH to each host (SSH key/agent).
- Remote has `make` and a C compiler.
- Uses `lscpu` (Linux) or `sysctl` (macOS) to name the CPU.

Hosts file: one host per line; blank lines and `#` comments allowed.

Common options:
- **`--remote-dir`**: Remote working dir (default: `~/cache_detect`).
- **`--output-dir`**: Local directory for results (default: script directory).
- **`--repo-dir`**: Local repository path to read sources from (default: script directory).
- **`-p/--port`**, **`-i/--identity`**: SSH port/key.
- **`--ssh-timeout`**, **`--build-timeout`**, **`--run-timeout`**: Timeouts.
- **`-j/--jobs`**: Parallel hosts (default: 4).
- **`--append`**: Append to existing output files.
- **`--ssh-option`**, **`--scp-option`**: Extra options (repeatable).
- **`--patterns`**: Comma-separated list or `all` (default: `all`). Patterns: `random,seq,reverse,stride,interleave,gray,bitrev`.

Examples:

```bash
# All patterns up to 1 GiB on each host, collecting logs locally
python3 sync_build_run.py hosts.txt \
  --remote-dir ~/cache_detect --output-dir ./logs --jobs 6 --patterns all

# Only random, seq, and stride
python3 sync_build_run.py hosts.txt --patterns random,seq,stride
```

Notes:
- The script builds once per host and runs each pattern separately.
- To change stride step or other `cache_detect` flags, edit the script or run manually on the remote.

### `plot_cache_logs.py` (visualization)

Plots one or more logs produced by `cache_detect` into an image and writes a combined CSV table of all series.

Options:
- **`-o/--output`**: Output image path (PNG/PDF/SVG). Default: `cache_plot.png`.
- **`--show`**: Show interactively instead of saving.
- **`--xunits`**: `bytes`, `KiB`, or `MiB` for the x-axis (default: `KiB`).
- **`--xlog`**: Logarithmic x-axis (base 2 ticks).
- **`--title`**: Plot title.
- **`--two-panels`**, **`--zoom-range A:B`**: Add a zoomed second subplot over `A..B` bytes.
- **`--third-panel`**, **`--third-range A:B`**: Optional third subplot.
- **`--table`**: Explicit CSV output path (default derives from image name).

Examples:

```bash
# Plot multiple CPU logs into one figure and CSV
python3 plot_cache_logs.py 4GB/Apple\ M4.txt 4GB/IBM\ POWER9.txt -o apple_vs_power9.png --xlog

# Plot pattern variants for one CPU
python3 plot_cache_logs.py "logs/Apple M1 Max (random).txt" "logs/Apple M1 Max (seq).txt" -o m1_patterns.png --xlog --two-panels --zoom-range 1KiB:4MiB
```

### Data files

- `4GB/*.txt` are example snapshots of full-range runs for reference. No other `.txt` files are tracked by default.
- CSVs and images are ignored by git; produce them locally as needed.

### License

This repository is provided as-is for benchmarking and educational use.
