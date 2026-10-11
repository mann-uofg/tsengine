#!/usr/bin/env python3
"""Turns the files written by run_all.sh into Markdown tables and charts.

usage: report.py <results dir> <image output dir>

Prints the tables to stdout. Every number comes straight from a result file; nothing is
computed here beyond medians, ratios and bytes per point.
"""

import glob
import json
import os
import re
import statistics
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

# One colour per system, the same in every chart.
COLOR = {
    "tsengine": "#2a78d6",
    "timescaledb": "#eb6834",
    "timescaledb-uncompressed": "#1baf7a",
}
NAME = {
    "tsengine": "tsengine",
    "timescaledb": "TimescaleDB, compressed",
    "timescaledb-uncompressed": "TimescaleDB, uncompressed",
}
SURFACE = "#fcfcfb"
TEXT = "#0b0b0b"
MUTED = "#52514e"
GRID = "#e4e3df"


def read_kv(path):
    values = {}
    with open(path) as f:
        for line in f:
            key, _, value = line.rstrip("\n").partition("=")
            values[key] = value
    return values


def load_runs(results, system):
    paths = sorted(glob.glob(os.path.join(results, f"load-{system}-run*.txt")))
    return [read_kv(p) for p in paths]


def read_queries(results, label):
    with open(os.path.join(results, f"queries-{label}.json")) as f:
        return {t["type"]: t for t in json.load(f)["types"]}


def official_median_ms(results, query_type):
    path = os.path.join(results, f"tsbs-official-{query_type}.txt")
    if not os.path.exists(path):
        return None
    with open(path) as f:
        match = re.search(r"med:\s*([0-9.]+)ms", f.read())
    return float(match.group(1)) if match else None


def style(ax):
    ax.set_facecolor(SURFACE)
    for side in ("top", "right", "left"):
        ax.spines[side].set_visible(False)
    ax.spines["bottom"].set_color(GRID)
    ax.tick_params(colors=MUTED, length=0, labelsize=9)
    ax.xaxis.grid(True, color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)


def hbars(ax, labels, values, colors, fmt, title, label_after=None):
    """Horizontal bars with the value written at the end of each.

    label_after gives, per bar, the x position the label must clear (for bars that also
    carry dots for single runs).
    """
    positions = range(len(values))
    ax.barh(positions, values, color=colors, height=0.55)
    ax.set_yticks(list(positions))
    ax.set_yticklabels(labels, color=TEXT, fontsize=9)
    ax.invert_yaxis()
    ends = label_after if label_after is not None else values
    limit = max(ends) * 1.22
    ax.set_xlim(0, limit)
    for position, value, end in zip(positions, values, ends):
        ax.text(end + limit * 0.015, position, fmt(value), va="center", color=TEXT, fontsize=9)
    ax.set_title(title, color=TEXT, fontsize=10, loc="left", pad=8)
    style(ax)


def save(fig, out_dir, name):
    fig.patch.set_facecolor(SURFACE)
    fig.savefig(os.path.join(out_dir, name), dpi=160, facecolor=SURFACE, bbox_inches="tight")
    plt.close(fig)


def main():
    results, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    env = read_kv(os.path.join(results, "environment.txt"))

    ts_loads = load_runs(results, "tsengine")
    pg_loads = load_runs(results, "timescaledb")
    points = int(ts_loads[-1]["points"])
    compress = read_kv(os.path.join(results, "compress-timescaledb-final.txt"))

    # ------------------------------------------------------------------ ingest
    rates = {
        "tsengine": [float(r["metrics_per_sec"]) for r in ts_loads],
        "timescaledb": [float(r["metrics_per_sec"]) for r in pg_loads],
    }
    print("## Ingest\n")
    print(f"{points:,} points, {env['load_workers']} loader workers, batches of "
          f"{env['load_batch_size']} rows, {len(ts_loads)} runs from an empty database.\n")
    print("| System | Median points/s | Min | Max |")
    print("|---|---|---|---|")
    for system in ("tsengine", "timescaledb"):
        r = rates[system]
        label = "TimescaleDB" if system == "timescaledb" else "tsengine"
        print(f"| {label} | {statistics.median(r):,.0f} | {min(r):,.0f} | {max(r):,.0f} |")
    print()

    fig, ax = plt.subplots(figsize=(7.2, 1.9))
    hbars(ax, ["tsengine", "TimescaleDB"],
          [statistics.median(rates["tsengine"]) / 1e6, statistics.median(rates["timescaledb"]) / 1e6],
          [COLOR["tsengine"], COLOR["timescaledb"]], lambda v: f"{v:.2f}M",
          f"Ingest rate, million points per second (median of {len(ts_loads)} runs; "
          "dots are single runs)",
          label_after=[max(rates["tsengine"]) / 1e6, max(rates["timescaledb"]) / 1e6])
    for position, system in enumerate(("tsengine", "timescaledb")):
        ax.scatter([r / 1e6 for r in rates[system]], [position] * len(rates[system]),
                   s=14, color=SURFACE, edgecolors=TEXT, linewidths=0.8, zorder=3)
    save(fig, out_dir, "ingest.png")

    # ----------------------------------------------------------------- storage
    sizes = {
        "tsengine": int(ts_loads[-1]["bytes_total"]),
        "timescaledb": int(compress["bytes_compressed"]) + int(pg_loads[-1]["bytes_tags"]),
        "timescaledb-uncompressed":
            int(compress["bytes_uncompressed"]) + int(pg_loads[-1]["bytes_tags"]),
    }
    print("## Storage\n")
    print("| System | Size on disk | Bytes per point |")
    print("|---|---|---|")
    for system in ("tsengine", "timescaledb", "timescaledb-uncompressed"):
        size = sizes[system]
        print(f"| {NAME[system]} | {size / 1e6:,.1f} MB | {size / points:.3f} |")
    print()
    print(f"tsengine block files alone: {int(ts_loads[-1]['bytes_blocks']) / points:.3f} "
          f"bytes per point. TimescaleDB compression took {float(compress['compress_seconds']):.1f} s; "
          f"tsengine's final compaction took {float(ts_loads[-1]['compact_seconds']):.1f} s.\n")

    fig, ax = plt.subplots(figsize=(7.2, 2.3))
    order = ["tsengine", "timescaledb", "timescaledb-uncompressed"]
    hbars(ax, [NAME[s] for s in order], [sizes[s] / points for s in order],
          [COLOR[s] for s in order], lambda v: f"{v:.2f}",
          "Size on disk, bytes per point")
    save(fig, out_dir, "storage.png")

    # ----------------------------------------------------------------- queries
    compressed = read_queries(results, "compressed")
    uncompressed = read_queries(results, "uncompressed")
    print("## Query latency\n")
    print("Milliseconds, one client, warm cache. p50 / p99 over "
          f"{next(iter(compressed.values()))['latency']['tsengine']['samples']:,} timed queries "
          "per system and type.\n")
    print("| Query | tsengine | TimescaleDB compressed | TimescaleDB uncompressed | "
          "Answers compared | Mismatches |")
    print("|---|---|---|---|---|---|")
    for query_type, report in compressed.items():
        ts = report["latency"]["tsengine"]
        pg = report["latency"]["timescaledb"]
        raw = uncompressed[query_type]["latency"]["timescaledb"]
        compared = report["verified"] + uncompressed[query_type]["verified"]
        mismatches = report["mismatches"] + uncompressed[query_type]["mismatches"]
        print(f"| `{query_type}` | {ts['p50_ms']:.2f} / {ts['p99_ms']:.2f} | "
              f"{pg['p50_ms']:.2f} / {pg['p99_ms']:.2f} | "
              f"{raw['p50_ms']:.2f} / {raw['p99_ms']:.2f} | {compared:,} | {mismatches} |")
    print()
    for query_type, report in compressed.items():
        print(f"- `{query_type}`: {report['about']}.")
    print()

    official = {t: official_median_ms(results, t) for t in compressed}
    if any(v is not None for v in official.values()):
        print("Check of this harness against the official TSBS runner "
              "(`tsbs_run_queries_timescaledb`, compressed table, median):\n")
        print("| Query | Official TSBS runner | This harness |")
        print("|---|---|---|")
        for query_type, median in official.items():
            if median is not None:
                mine = compressed[query_type]["latency"]["timescaledb"]["p50_ms"]
                print(f"| `{query_type}` | {median:.2f} ms | {mine:.2f} ms |")
        print()

    types = list(compressed)
    fig, axes = plt.subplots(len(types), 1, figsize=(7.2, 1.45 * len(types)))
    for ax, query_type in zip(axes, types):
        order = ["tsengine", "timescaledb", "timescaledb-uncompressed"]
        latencies = [
            compressed[query_type]["latency"]["tsengine"],
            compressed[query_type]["latency"]["timescaledb"],
            uncompressed[query_type]["latency"]["timescaledb"],
        ]
        hbars(ax, [NAME[s] for s in order], [l["p50_ms"] for l in latencies],
              [COLOR[s] for s in order], lambda v: f"{v:.2f} ms",
              f"{query_type}: {compressed[query_type]['about']}")
    fig.suptitle("Query latency, median (p50), milliseconds. Each panel has its own scale.",
                 color=TEXT, fontsize=10, x=0.02, ha="left")
    fig.tight_layout(rect=(0, 0, 1, 0.97))
    save(fig, out_dir, "queries.png")

    # ---------------------------------------------------------------- block size
    block_rows = [(1024, ts_loads[-1], compressed)]
    for size in (360, 128):
        load_path = os.path.join(results, f"load-tsengine-blocks{size}.txt")
        if os.path.exists(load_path):
            block_rows.append((size, read_kv(load_path), read_queries(results, f"blocks{size}")))
    if len(block_rows) > 1:
        print("## Block size and aggregation push-down (tsengine)\n")
        print("| Points per block | Bytes per point | `double-groupby-1` p50 | "
              "blocks from statistics | `cpu-max-all-1` p50 | blocks from statistics | "
              "`single-groupby-1-1-12` p50 |")
        print("|---|---|---|---|---|---|---|")
        for size, load, queries in block_rows:
            cells = [str(size), f"{int(load['bytes_total']) / points:.3f}"]
            for query_type in ("double-groupby-1", "cpu-max-all-1"):
                report = queries[query_type]
                total = report["tsengine_blocks_from_stats"] + report["tsengine_blocks_decoded"]
                share = 100 * report["tsengine_blocks_from_stats"] / total if total else 0
                cells.append(f"{report['latency']['tsengine']['p50_ms']:.2f} ms")
                cells.append(f"{share:.0f}%")
            cells.append(f"{queries['single-groupby-1-1-12']['latency']['tsengine']['p50_ms']:.2f} ms")
            print("| " + " | ".join(cells) + " |")
        print()

        fig, axes = plt.subplots(1, 2, figsize=(7.2, 2.1))
        labels = [f"{size} points" for size, _, _ in block_rows]
        blue = [COLOR["tsengine"]] * len(block_rows)
        hbars(axes[0], labels, [int(load["bytes_total"]) / points for _, load, _ in block_rows],
              blue, lambda v: f"{v:.2f}", "Bytes per point")
        hbars(axes[1], labels,
              [q["double-groupby-1"]["latency"]["tsengine"]["p50_ms"] for _, _, q in block_rows],
              blue, lambda v: f"{v:.2f} ms", "double-groupby-1, p50")
        fig.suptitle("tsengine: effect of block size", color=TEXT, fontsize=10, x=0.02, ha="left")
        fig.tight_layout(rect=(0, 0, 1, 0.93))
        save(fig, out_dir, "block_size.png")

    # ------------------------------------------------------------- environment
    print("## Environment\n")
    for key in ("date", "cpu", "cpus", "memory_kb", "os", "kernel", "filesystem", "docker",
                "timescale_image", "tsbs_commit", "tsengine_commit", "tsengine_dirty",
                "compiler", "go", "scale", "ts_start", "ts_end", "log_interval"):
        print(f"- {key}: {env.get(key, '?')}")
    print(f"- timescaledb shared_buffers: {pg_loads[-1].get('shared_buffers', '?')}")
    print(f"- timescaledb container memory after load: {pg_loads[-1].get('container_memory', '?')}")
    print(f"- timescaledb container memory after queries: "
          f"{env.get('container_memory_after_queries', '?')}")
    print(f"- tsengine peak resident memory during load: "
          f"{int(ts_loads[-1]['peak_rss_kb_after_load']) / 1024:.0f} MB")


if __name__ == "__main__":
    main()
