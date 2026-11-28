#!/usr/bin/env python3
"""Benchmarks for the search server, driven through its HTTP API.

Subcommands:
  index  Index build time versus number of threads (POST /api/index).
"""
import argparse
import csv
import http.client
import json
import os
import statistics
import sys
import urllib.parse

DEFAULT_THREADS = "1,2,4,6,8,10,12,14,16,24,32"
WARMUP_RUNS = 1
MEASURED_RUNS = 3
INDEX_TIMEOUT_S = 3600
DEFAULT_OUT = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "benchmarks", "results")


def parse_int_list(text):
    try:
        values = [int(part) for part in text.split(",") if part.strip()]
    except ValueError:
        raise argparse.ArgumentTypeError("expected comma-separated integers")
    if not values or any(v < 1 for v in values):
        raise argparse.ArgumentTypeError("values must be integers >= 1")
    return values


def with_suffix(name, suffix):
    if not suffix:
        return name
    stem, ext = os.path.splitext(name)
    return "%s_%s%s" % (stem, suffix, ext)


def post_index(host, threads):
    """Calls POST /api/index and returns time_ms from the response."""
    url = urllib.parse.urlparse(host)
    conn = http.client.HTTPConnection(url.hostname, url.port or 80,
                                      timeout=INDEX_TIMEOUT_S)
    try:
        body = json.dumps({"threads": threads})
        conn.request("POST", "/api/index", body,
                     {"Content-Type": "application/json"})
        resp = conn.getresponse()
        data = resp.read().decode("utf-8", "replace")
        if resp.status != 200:
            raise RuntimeError("POST /api/index (threads=%d) -> HTTP %d: %s" %
                               (threads, resp.status, data))
        return int(json.loads(data)["time_ms"])
    finally:
        conn.close()


def summarize(runs):
    """runs: {threads: [time_ms,...]} -> rows with mean/min/max/S/E."""
    rows = []
    t1 = statistics.mean(runs[1]) if 1 in runs else None
    for n in sorted(runs):
        mean = statistics.mean(runs[n])
        row = {"threads": n, "mean_ms": mean, "min_ms": min(runs[n]),
               "max_ms": max(runs[n]), "speedup": None, "efficiency": None}
        if t1 is not None and mean > 0:
            row["speedup"] = t1 / mean
            row["efficiency"] = row["speedup"] / n
        rows.append(row)
    return rows


def fmt(value, digits):
    return "" if value is None else ("%.*f" % (digits, value))


def write_csvs(out_dir, suffix, runs, rows):
    runs_path = os.path.join(out_dir, with_suffix("index_runs.csv", suffix))
    with open(runs_path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["threads", "run", "time_ms"])
        for n in sorted(runs):
            for i, t in enumerate(runs[n], 1):
                w.writerow([n, i, t])
    summary_path = os.path.join(out_dir,
                                with_suffix("index_summary.csv", suffix))
    with open(summary_path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["threads", "mean_ms", "min_ms", "max_ms", "speedup",
                    "efficiency"])
        for r in rows:
            w.writerow([r["threads"], fmt(r["mean_ms"], 2), r["min_ms"],
                        r["max_ms"], fmt(r["speedup"], 4),
                        fmt(r["efficiency"], 4)])
    return runs_path, summary_path


def plot_index(out_dir, suffix, rows):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    threads = [r["threads"] for r in rows]
    fig, axes = plt.subplots(1, 3, figsize=(15, 4.5))

    ax = axes[0]
    means = [r["mean_ms"] for r in rows]
    ax.plot(threads, means, "o-", label="mean")
    ax.fill_between(threads, [r["min_ms"] for r in rows],
                    [r["max_ms"] for r in rows], alpha=0.2, label="min..max")
    ax.set_xlabel("threads")
    ax.set_ylabel("index build time, ms")
    ax.set_title("Index build time")
    ax.legend()

    have_speedup = all(r["speedup"] is not None for r in rows)
    ax = axes[1]
    if have_speedup:
        ax.plot(threads, [r["speedup"] for r in rows], "o-", label="S(N)")
        ax.plot(threads, threads, "k--", alpha=0.5, label="ideal")
        ax.legend()
    else:
        ax.text(0.5, 0.5, "no N=1 measurement", ha="center", va="center",
                transform=ax.transAxes)
    ax.set_xlabel("threads")
    ax.set_ylabel("speedup S(N) = T1 / TN")
    ax.set_title("Speedup")

    ax = axes[2]
    if have_speedup:
        ax.plot(threads, [r["efficiency"] for r in rows], "o-")
        ax.set_ylim(bottom=0)
    else:
        ax.text(0.5, 0.5, "no N=1 measurement", ha="center", va="center",
                transform=ax.transAxes)
    ax.set_xlabel("threads")
    ax.set_ylabel("efficiency E(N) = S(N) / N")
    ax.set_title("Efficiency")

    for a in axes:
        a.grid(True, alpha=0.3)
    fig.tight_layout()
    path = os.path.join(out_dir, with_suffix("index_time_vs_threads.png",
                                             suffix))
    fig.savefig(path, dpi=150)
    plt.close(fig)
    return path


def cmd_index(args):
    os.makedirs(args.out, exist_ok=True)
    runs = {}
    for n in args.threads:
        for _ in range(args.warmup):
            t = post_index(args.host, n)
            print("threads=%-3d warmup  %d ms (discarded)" % (n, t),
                  flush=True)
        runs[n] = []
        for i in range(1, args.runs + 1):
            t = post_index(args.host, n)
            runs[n].append(t)
            print("threads=%-3d run %d   %d ms" % (n, i, t), flush=True)
    rows = summarize(runs)
    runs_path, summary_path = write_csvs(args.out, args.suffix, runs, rows)
    png_path = plot_index(args.out, args.suffix, rows)
    print("\nthreads  mean_ms   min_ms   max_ms  speedup  efficiency")
    for r in rows:
        print("%7d %8.1f %8d %8d %8s %11s" %
              (r["threads"], r["mean_ms"], r["min_ms"], r["max_ms"],
               fmt(r["speedup"], 2), fmt(r["efficiency"], 3)))
    if 1 not in runs:
        print("warning: N=1 not in the thread list; speedup/efficiency "
              "are empty", file=sys.stderr)
    print("\nwrote %s\n      %s\n      %s" %
          (runs_path, summary_path, png_path))
    return 0


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("index", help="index build time vs threads")
    p.add_argument("--host", default="http://localhost:8080",
                   help="server URL (default: %(default)s)")
    p.add_argument("--threads", type=parse_int_list, default=parse_int_list(DEFAULT_THREADS),
                   help="comma-separated thread counts (default: %s)" %
                   DEFAULT_THREADS)
    p.add_argument("--warmup", type=int, default=WARMUP_RUNS,
                   help="discarded warm-up runs per N (default: %(default)s)")
    p.add_argument("--runs", type=int, default=MEASURED_RUNS,
                   help="measured runs per N (default: %(default)s)")
    p.add_argument("--out", default=DEFAULT_OUT,
                   help="output directory (default: benchmarks/results)")
    p.add_argument("--suffix", default="",
                   help="adds _SUFFIX to the names of all output files")
    p.set_defaults(func=cmd_index)
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except (OSError, RuntimeError, http.client.HTTPException) as e:
        print("error: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
