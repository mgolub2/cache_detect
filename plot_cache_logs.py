#!/usr/bin/env python3
import argparse
import os
import math
import itertools
import csv
from pathlib import Path
from typing import List, Tuple, Optional, Sequence


def parse_log(path: Path) -> Tuple[List[float], List[float]]:
    sizes: List[float] = []
    lats: List[float] = []
    with path.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            s = line.strip()
            if not s:
                # stop at the blank line before the "Detected cache levels" section
                break
            if s.startswith("#"):
                continue
            if s.lower().startswith("detected cache levels"):
                break
            parts = s.split()
            if len(parts) < 2:
                continue
            try:
                size = float(parts[0])
                lat = float(parts[1])
            except ValueError:
                continue
            sizes.append(size)
            lats.append(lat)
    return sizes, lats


def main() -> None:
    parser = argparse.ArgumentParser(description="Plot cache timing logs from cache_detect.")
    parser.add_argument("logs", nargs="+", help="Paths to timing log files. Label = file basename (CPU name)")
    parser.add_argument("-o", "--output", default="cache_plot.png", help="Output image path (PNG/PDF/SVG)")
    parser.add_argument("--show", action="store_true", help="Show the plot interactively instead of saving")
    parser.add_argument("--xunits", choices=["bytes", "KiB", "MiB"], default="KiB", help="Units for x-axis")
    parser.add_argument("--xlog", action="store_true", help="Use logarithmic x-axis")
    parser.add_argument("--title", default="Cache Latency vs Working-Set Size", help="Plot title")
    parser.add_argument(
        "--two-panels",
        action="store_true",
        help="Add a second subplot zoomed to --zoom-range",
    )
    parser.add_argument(
        "--zoom-range",
        default="1KiB:4MiB",
        help="Zoomed x-range for second subplot (e.g., '1KiB:4MiB' or '2048:4194304')",
    )
    parser.add_argument(
        "--third-panel",
        action="store_true",
        help="Add a third subplot zoomed to --third-range (combine with --two-panels for three subplots)",
    )
    parser.add_argument(
        "--third-range",
        default="256KiB:64MiB",
        help="Zoomed x-range for third subplot (e.g., '256KiB:64MiB' or '262144:67108864')",
    )
    parser.add_argument(
        "--table",
        default=None,
        help="Output CSV table path (defaults to replacing image suffix with .csv)",
    )
    args = parser.parse_args()

    # Choose backend lazily: use Agg unless showing
    import matplotlib
    if not args.show:
        matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.ticker import FuncFormatter, LogLocator, MaxNLocator, FixedLocator, NullFormatter

    unit_div = {"bytes": 1.0, "KiB": 1024.0, "MiB": 1024.0 * 1024.0}[args.xunits]

    def _parse_size_to_bytes(text: str) -> Optional[float]:
        s = text.strip().replace(" ", "")
        if not s:
            return None
        try:
            return float(s)
        except ValueError:
            pass
        # accept KB/MB as decimal and KiB/MiB as binary; case-insensitive
        suffixes = {
            "b": 1.0,
            "kb": 1e3,
            "mb": 1e6,
            "gb": 1e9,
            "kib": 1024.0,
            "mib": 1024.0 * 1024.0,
            "gib": 1024.0 * 1024.0 * 1024.0,
        }
        s_lower = s.lower()
        for suf, mul in suffixes.items():
            if s_lower.endswith(suf):
                try:
                    v = float(s_lower[: -len(suf)])
                    return v * mul
                except ValueError:
                    return None
        return None

    def _format_size_tick(x, _pos=None):
        # Convert current axis value (in args.xunits) back to bytes for formatting
        bytes_val = float(x) * unit_div
        units = ["B", "KiB", "MiB", "GiB", "TiB"]
        u = 0
        v = bytes_val
        while v >= 1024.0 and u < len(units) - 1:
            v /= 1024.0
            u += 1
        # For small non-integers use one decimal; otherwise no decimals
        if v < 10 and abs(v - round(v)) > 1e-9:
            return f"{v:.1f} {units[u]}"
        return f"{v:.0f} {units[u]}"

    # Prepare figure/axes
    num_panels = 1 + int(args.two_panels) + int(args.third_panel)
    if num_panels == 1:
        fig, ax = plt.subplots(figsize=(9, 5.5), dpi=120)
        ax_zoom = None
        ax_third = None
    else:
        fig_h = 5.5 + 3.5 * (num_panels - 1)
        fig, axes = plt.subplots(num_panels, 1, figsize=(9, fig_h), dpi=120, sharex=False, sharey=False)
        ax = axes[0]
        next_idx = 1
        ax_zoom = axes[next_idx] if args.two_panels else None
        if args.two_panels:
            next_idx += 1
        ax_third = axes[next_idx] if args.third_panel else None

    any_data = False
    data_min_b = float('inf')
    data_max_b = 0.0
    series: List[Tuple[str, Sequence[float], Sequence[float]]] = []
    for log in args.logs:
        path = Path(log)
        label = path.stem
        sizes_b, lats_ns = parse_log(path)
        if not sizes_b:
            continue
        series.append((label, sizes_b, lats_ns))
        any_data = True
        lb = min(sizes_b)
        ub = max(sizes_b)
        if lb < data_min_b:
            data_min_b = lb
        if ub > data_max_b:
            data_max_b = ub

    if not any_data:
        raise SystemExit("No plottable data found in inputs.")

    # Prepare style cycle: 7 markers x 5 colors = 35 combos
    colors = ["C0", "C1", "C2", "C3", "C4"]
    markers = ["o", "s", "^", "v", "D", "<", ">"]
    combos = [(c, m) for m in markers for c in colors]
    style_cycle = itertools.cycle(combos)
    style_map = {label: next(style_cycle) for (label, _sb, _lns) in series}

    # Plot full-range axis
    for label, sizes_b, lats_ns in series:
        x = [s / unit_div for s in sizes_b]
        color, marker = style_map[label]
        ax.plot(x, lats_ns, marker=marker, color=color, markersize=3, linewidth=1.2, label=label)
    ax.set_xlabel("Working-set size")
    ax.set_ylabel("Latency (ns per pointer access)")
    ax.grid(True, which="both", linestyle=":", linewidth=0.6)
    ax.legend(title="CPU")
    def _setup_axis(axh, lo_b: Optional[float], hi_b: Optional[float]) -> None:
        if args.xlog:
            # Ensure the logarithmic scale uses base 2
            try:
                axh.set_xscale("log", base=2)
            except TypeError:
                axh.set_xscale("log", basex=2)
            # Determine tick range from provided limits or data
            lo = max(1.0, float(lo_b if lo_b is not None else data_min_b))
            hi = max(lo * 2.0, float(hi_b if hi_b is not None else data_max_b))
            p_lo = int(math.floor(math.log(lo, 2)))
            p_hi = int(math.ceil(math.log(hi, 2)))
            ticks_b = [float(2 ** p) for p in range(p_lo, p_hi + 1)]
            # Cap number of labels
            max_labels = 16
            if len(ticks_b) > max_labels:
                step = int(math.ceil(len(ticks_b) / max_labels))
                ticks_b = ticks_b[::step]
            ticks_x = [tb / unit_div for tb in ticks_b]
            axh.xaxis.set_major_locator(FixedLocator(ticks_x))
            axh.xaxis.set_minor_locator(FixedLocator([]))
            axh.xaxis.set_minor_formatter(NullFormatter())
            labels = [_format_size_tick(tx) for tx in ticks_x]
            try:
                axh.set_xticks(ticks_x, labels=labels)
            except TypeError:
                axh.set_xticks(ticks_x)
                axh.set_xticklabels(labels)
            if lo_b is not None or hi_b is not None:
                lo_x = (lo_b if lo_b is not None else data_min_b) / unit_div
                hi_x = (hi_b if hi_b is not None else data_max_b) / unit_div
                axh.set_xlim(lo_x, hi_x)
        else:
            axh.xaxis.set_major_locator(MaxNLocator(nbins=8))
            axh.minorticks_on()
            if lo_b is not None or hi_b is not None:
                lo_x = (lo_b if lo_b is not None else data_min_b) / unit_div
                hi_x = (hi_b if hi_b is not None else data_max_b) / unit_div
                axh.set_xlim(lo_x, hi_x)
        # Ensure human-readable labels
        axh.xaxis.set_major_formatter(FuncFormatter(_format_size_tick))

    # Full axis setup
    _setup_axis(ax, None, None)

    # Zoomed subplot
    if ax_zoom is not None:
        # parse zoom range
        if ":" in args.zoom_range:
            lo_txt, hi_txt = args.zoom_range.split(":", 1)
            lo_b = _parse_size_to_bytes(lo_txt) or 1024.0
            hi_b = _parse_size_to_bytes(hi_txt) or (4 * 1024.0 * 1024.0)
        else:
            lo_b = 1024.0
            hi_b = 4 * 1024.0 * 1024.0
        # plot only in-range data and compute local y-limits
        y_min = float('inf')
        y_max = float('-inf')
        for label, sizes_b, lats_ns in series:
            filtered = [(s, y) for s, y in zip(sizes_b, lats_ns) if (s >= lo_b and s <= hi_b)]
            if not filtered:
                continue
            x = [s / unit_div for s, _ in filtered]
            y = [y for _, y in filtered]
            y_min = min(y_min, min(y))
            y_max = max(y_max, max(y))
            color, marker = style_map[label]
            ax_zoom.plot(x, y, marker=marker, color=color, markersize=3, linewidth=1.2, label=label)
        ax_zoom.set_xlabel("Working-set size")
        ax_zoom.set_ylabel("Latency (ns per pointer access)")
        ax_zoom.grid(True, which="both", linestyle=":", linewidth=0.6)
        _setup_axis(ax_zoom, lo_b, hi_b)
        if y_min < float('inf') and y_max > float('-inf'):
            pad = 0.05 * (y_max - y_min) if y_max > y_min else (0.05 * y_max if y_max > 0 else 1.0)
            lo_y = max(0.0, y_min - pad)
            hi_y = y_max + pad
            if hi_y <= lo_y:
                hi_y = lo_y + 1.0
            ax_zoom.set_ylim(lo_y, hi_y)

    # Third subplot
    if 'ax_third' in locals() and ax_third is not None:
        # parse third range
        if ":" in args.third_range:
            lo_txt, hi_txt = args.third_range.split(":", 1)
            lo_b = _parse_size_to_bytes(lo_txt) or 1024.0
            hi_b = _parse_size_to_bytes(hi_txt) or (4 * 1024.0 * 1024.0)
        else:
            lo_b = 1024.0
            hi_b = 4 * 1024.0 * 1024.0
        # plot only in-range data and compute local y-limits
        y_min = float('inf')
        y_max = float('-inf')
        for label, sizes_b, lats_ns in series:
            filtered = [(s, y) for s, y in zip(sizes_b, lats_ns) if (s >= lo_b and s <= hi_b)]
            if not filtered:
                continue
            x = [s / unit_div for s, _ in filtered]
            y = [y for _, y in filtered]
            y_min = min(y_min, min(y))
            y_max = max(y_max, max(y))
            color, marker = style_map[label]
            ax_third.plot(x, y, marker=marker, color=color, markersize=3, linewidth=1.2, label=label)
        ax_third.set_xlabel("Working-set size")
        ax_third.set_ylabel("Latency (ns per pointer access)")
        ax_third.grid(True, which="both", linestyle=":", linewidth=0.6)
        _setup_axis(ax_third, lo_b, hi_b)
        if y_min < float('inf') and y_max > float('-inf'):
            pad = 0.05 * (y_max - y_min) if y_max > y_min else (0.05 * y_max if y_max > 0 else 1.0)
            lo_y = max(0.0, y_min - pad)
            hi_y = y_max + pad
            if hi_y <= lo_y:
                hi_y = lo_y + 1.0
            ax_third.set_ylim(lo_y, hi_y)

    # Global title
    fig.suptitle(args.title)

    # Write combined CSV table of all plotted points
    table_out = Path(args.table) if args.table else Path(args.output).with_suffix(".csv")
    table_out.parent.mkdir(parents=True, exist_ok=True)
    with table_out.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["cpu", "size_bytes", f"size_{args.xunits}", "latency_ns"]) 
        for label, sizes_b, lats_ns in series:
            for s_b, lat in zip(sizes_b, lats_ns):
                writer.writerow([label, s_b, s_b / unit_div, lat])

    fig.tight_layout()
    if args.show:
        plt.show()
    else:
        out = Path(args.output)
        out.parent.mkdir(parents=True, exist_ok=True)
        fig.savefig(out)
        print(f"Wrote {out}")
        print(f"Wrote {table_out}")


if __name__ == "__main__":
    main()


