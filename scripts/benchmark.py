#!/usr/bin/env python3
"""Benchmarks for the search server, driven through its HTTP API.

Subcommands:
  index       Index build time versus number of threads (POST /api/index).
  throughput  Requests per second versus number of concurrent clients:
              (a) POST /api/search with EXPANSION=off, (b) full pipeline
              through WS /ws/search.
"""
import argparse
import asyncio
import csv
import http.client
import json
import math
import os
import re
import statistics
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse

DEFAULT_THREADS = "1,2,4,6,8,10,12,14,16,24,32"
WARMUP_RUNS = 1
MEASURED_RUNS = 3
INDEX_TIMEOUT_S = 3600
DEFAULT_CLIENTS = "1,2,5,10,20,50"
DEFAULT_DURATION_S = 30
DEFAULT_INDEX_THREADS = 10
HEALTH_WAIT_S = 120
REQUEST_TIMEOUT_S = 60
REPO_ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
CURVE_REST = "rest_no_expansion"
CURVE_WS = "ws_full"
# Fixed query set, identical for every point of every curve.
QUERIES = [
    "ocean voyage", "ghost", "war", "king and queen", "love and death",
    "old castle", "river", "ship captain", "garden flowers", "battle",
    "kingdom", "storm at sea", "кораблі в морі", "nonexistentwordzzqx",
]
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


# ---------------------------------------------------------------- throughput

_TOKEN_RE = re.compile(r"[A-Za-z0-9]+")


def tokenize(text):
    """Same rule as the server Tokenizer: [A-Za-z0-9]+, length 2..64,
    lowercase, unique in order of appearance."""
    seen, terms = set(), []
    for m in _TOKEN_RE.finditer(text):
        t = m.group(0)
        if 2 <= len(t) <= 64:
            t = t.lower()
            if t not in seen:
                seen.add(t)
                terms.append(t)
    return terms


def percentile(sorted_values, p):
    """Nearest-rank percentile of an ascending list."""
    if not sorted_values:
        return None
    rank = max(1, math.ceil(p / 100.0 * len(sorted_values)))
    return sorted_values[rank - 1]


def point_metrics(curve, clients, duration_s, latencies, errors, elapsed,
                  fallbacks):
    lat = sorted(latencies)
    ok = len(lat)
    row = {
        "curve": curve, "clients": clients, "duration_s": duration_s,
        "requests": ok + errors, "errors": errors,
        "rps": (ok / elapsed) if elapsed > 0 else 0.0,
        "median_ms": statistics.median(lat) if lat else None,
        "p95_ms": percentile(lat, 95),
        "fallback_share": None,
    }
    if fallbacks is not None and ok:
        row["fallback_share"] = fallbacks / ok
    return row


def run_rest_point(host, clients, duration_s):
    """Closed loop: each client thread sends the next POST /api/search after
    the previous response (one keep-alive connection per client)."""
    url = urllib.parse.urlparse(host)
    port = url.port or 80
    deadline = time.monotonic() + duration_s
    start = time.monotonic()
    results = [None] * clients

    def worker(idx):
        lat, errors = [], 0
        conn = None
        i = idx
        while time.monotonic() < deadline:
            query = QUERIES[i % len(QUERIES)]
            i += 1
            body = json.dumps({"query": query, "mode": "keyword"})
            t0 = time.perf_counter()
            try:
                if conn is None:
                    conn = http.client.HTTPConnection(
                        url.hostname, port, timeout=REQUEST_TIMEOUT_S)
                conn.request("POST", "/api/search", body,
                             {"Content-Type": "application/json"})
                resp = conn.getresponse()
                resp.read()
                if resp.status == 200:
                    lat.append((time.perf_counter() - t0) * 1000.0)
                else:
                    errors += 1
                if resp.will_close:
                    conn.close()
                    conn = None
            except (OSError, http.client.HTTPException):
                errors += 1
                if conn is not None:
                    conn.close()
                conn = None
                time.sleep(0.01)
        if conn is not None:
            conn.close()
        results[idx] = (lat, errors)

    threads = [threading.Thread(target=worker, args=(k,))
               for k in range(clients)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    elapsed = time.monotonic() - start
    lat = [x for r in results for x in r[0]]
    errors = sum(r[1] for r in results)
    return point_metrics(CURVE_REST, clients, duration_s, lat, errors,
                         elapsed, None)


async def _ws_client(ws_url, idx, deadline, stats):
    import websockets
    i = idx
    while time.monotonic() < deadline:
        query = QUERIES[i % len(QUERIES)]
        i += 1
        t0 = time.perf_counter()
        try:
            async with websockets.connect(
                    ws_url, open_timeout=REQUEST_TIMEOUT_S,
                    max_size=None) as ws:
                await ws.send(json.dumps({"query": query}))
                expanded = None
                while True:
                    raw = await asyncio.wait_for(ws.recv(),
                                                 REQUEST_TIMEOUT_S)
                    msg = json.loads(raw)
                    stage = msg.get("stage")
                    if stage == "query_expansion":
                        expanded = msg["data"]["expanded_terms"]
                    elif stage == "done":
                        break
                    elif stage == "error":
                        raise RuntimeError(msg.get("data"))
            stats["lat"].append((time.perf_counter() - t0) * 1000.0)
            if expanded == tokenize(query):
                stats["fallbacks"] += 1
        except (OSError, RuntimeError, ValueError, KeyError, TypeError,
                asyncio.TimeoutError,
                websockets.exceptions.WebSocketException):
            stats["errors"] += 1
            await asyncio.sleep(0.01)


def run_ws_point(host, clients, duration_s):
    url = urllib.parse.urlparse(host)
    ws_url = "%s://%s/ws/search" % ("wss" if url.scheme == "https" else "ws",
                                    url.netloc)
    stats = {"lat": [], "errors": 0, "fallbacks": 0}

    async def run():
        deadline = time.monotonic() + duration_s
        await asyncio.gather(*[_ws_client(ws_url, k, deadline, stats)
                               for k in range(clients)])

    start = time.monotonic()
    asyncio.run(run())
    elapsed = time.monotonic() - start
    return point_metrics(CURVE_WS, clients, duration_s, stats["lat"],
                         stats["errors"], elapsed, stats["fallbacks"])


def http_get_json(host, path, timeout=2):
    url = urllib.parse.urlparse(host)
    conn = http.client.HTTPConnection(url.hostname, url.port or 80,
                                      timeout=timeout)
    try:
        conn.request("GET", path)
        resp = conn.getresponse()
        data = resp.read().decode("utf-8", "replace")
        if resp.status != 200:
            raise RuntimeError("GET %s -> HTTP %d" % (path, resp.status))
        return json.loads(data)
    finally:
        conn.close()


def wait_health(host, proc=None):
    deadline = time.monotonic() + HEALTH_WAIT_S
    while time.monotonic() < deadline:
        if proc is not None and proc.poll() is not None:
            raise RuntimeError("server exited with code %d" % proc.returncode)
        try:
            return http_get_json(host, "/api/health")
        except (OSError, RuntimeError, ValueError, http.client.HTTPException):
            time.sleep(0.5)
    raise RuntimeError("server did not become healthy in %d s" %
                       HEALTH_WAIT_S)


class ServerController:
    """Restarts the server with the given EXPANSION value (the variable is
    read once at server start) and re-indexes."""

    def __init__(self, args):
        self.args = args
        self.proc = None
        self.log = None

    def restart(self, expansion):
        args = self.args
        if args.server == "spawn":
            self.stop()
            port = urllib.parse.urlparse(args.host).port or 80
            env = dict(os.environ)
            env.update({"PORT": str(port), "EXPANSION": expansion})
            for key, value in (("DATASET_DIR", args.dataset),
                               ("WEB_DIR", args.web_dir),
                               ("OLLAMA_HOST", args.ollama_host)):
                if value:
                    env[key] = value
            self.log = tempfile.TemporaryFile()
            print("spawning %s (EXPANSION=%s)" % (args.server_bin, expansion),
                  flush=True)
            self.proc = subprocess.Popen([args.server_bin], env=env,
                                         stdout=self.log, stderr=self.log)
        else:
            env = dict(os.environ, EXPANSION=expansion)
            print("docker compose up -d --force-recreate search-server "
                  "(EXPANSION=%s)" % expansion, flush=True)
            subprocess.run(["docker", "compose", "up", "-d",
                            "--force-recreate", "search-server"],
                           cwd=REPO_ROOT, env=env, check=True)
        wait_health(args.host, self.proc)
        t = post_index(args.host, args.index_threads)
        print("indexed with %d threads in %d ms" % (args.index_threads, t),
              flush=True)

    def stop(self):
        if self.proc is not None:
            if self.proc.poll() is None:
                self.proc.terminate()
                try:
                    self.proc.wait(30)
                except subprocess.TimeoutExpired:
                    self.proc.kill()
                    self.proc.wait()
            self.proc = None
        if self.log is not None:
            self.log.close()
            self.log = None


def write_throughput_csv(out_dir, rows):
    path = os.path.join(out_dir, "throughput.csv")
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["curve", "clients", "duration_s", "requests", "errors",
                    "rps", "median_ms", "p95_ms", "fallback_share"])
        for r in rows:
            w.writerow([r["curve"], r["clients"], r["duration_s"],
                        r["requests"], r["errors"], fmt(r["rps"], 2),
                        fmt(r["median_ms"], 2), fmt(r["p95_ms"], 2),
                        fmt(r["fallback_share"], 4)])
    return path


def plot_throughput(out_dir, rows):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    labels = {CURVE_REST: "REST, EXPANSION=off",
              CURVE_WS: "WebSocket, full pipeline (Ollama)"}
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.5))
    for curve in (CURVE_REST, CURVE_WS):
        pts = [r for r in rows if r["curve"] == curve]
        if not pts:
            continue
        xs = [r["clients"] for r in pts]
        axes[0].plot(xs, [r["rps"] for r in pts], "o-", label=labels[curve])
        axes[1].plot(xs, [r["p95_ms"] or 0 for r in pts], "o-",
                     label=labels[curve])
    axes[0].set_ylabel("throughput, requests/s")
    axes[0].set_title("Throughput")
    axes[1].set_ylabel("p95 latency, ms")
    axes[1].set_title("p95 latency")
    axes[1].set_yscale("symlog")
    for a in axes:
        a.set_xlabel("concurrent clients")
        a.grid(True, alpha=0.3)
        a.legend()
    fig.tight_layout()
    path = os.path.join(out_dir, "throughput_vs_clients.png")
    fig.savefig(path, dpi=150)
    plt.close(fig)
    return path


def cmd_throughput(args):
    os.makedirs(args.out, exist_ok=True)
    curves = [c.strip() for c in args.curves.split(",") if c.strip()]
    if not curves or any(c not in ("rest", "ws") for c in curves):
        raise RuntimeError("--curves must be a list of: rest, ws")
    ctl = ServerController(args)
    rows = []
    try:
        for curve in curves:
            if curve == "rest":
                ctl.restart("off")
                run_point = run_rest_point
            else:
                ctl.restart("on")
                if not wait_health(args.host).get("ollama"):
                    print("warning: Ollama is not available; the WS curve "
                          "will measure fallback", file=sys.stderr)
                run_point = run_ws_point
            for n in args.clients:
                r = run_point(args.host, n, args.duration)
                rows.append(r)
                print("%-18s clients=%-3d requests=%-6d errors=%-4d "
                      "rps=%8.1f median=%8s p95=%8s fallback=%s" %
                      (r["curve"], n, r["requests"], r["errors"], r["rps"],
                       fmt(r["median_ms"], 1), fmt(r["p95_ms"], 1),
                       fmt(r["fallback_share"], 3) or "-"), flush=True)
    finally:
        if args.server == "spawn":
            ctl.stop()
    csv_path = write_throughput_csv(args.out, rows)
    png_path = plot_throughput(args.out, rows)
    print("\nwrote %s\n      %s" % (csv_path, png_path))
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

    p = sub.add_parser("throughput", help="requests/s vs concurrent clients")
    p.add_argument("--host", default="http://localhost:8080",
                   help="server URL (default: %(default)s)")
    p.add_argument("--clients", type=parse_int_list,
                   default=parse_int_list(DEFAULT_CLIENTS),
                   help="comma-separated client counts (default: %s)" %
                   DEFAULT_CLIENTS)
    p.add_argument("--duration", type=float, default=DEFAULT_DURATION_S,
                   help="seconds per point (default: %(default)s)")
    p.add_argument("--curves", default="rest,ws",
                   help="curves to measure, in order: rest (EXPANSION=off), "
                   "ws (EXPANSION=on) (default: %(default)s)")
    p.add_argument("--server", choices=("spawn", "docker"), default="spawn",
                   help="how the server is (re)started for each curve "
                   "(default: %(default)s)")
    p.add_argument("--server-bin",
                   default=os.path.join(REPO_ROOT, "build", "search_server"),
                   help="server binary for --server spawn")
    p.add_argument("--dataset", default="",
                   help="DATASET_DIR for --server spawn (default: server's)")
    p.add_argument("--web-dir", default="",
                   help="WEB_DIR for --server spawn (default: server's)")
    p.add_argument("--ollama-host", default="",
                   help="OLLAMA_HOST for --server spawn (default: server's)")
    p.add_argument("--index-threads", type=int,
                   default=DEFAULT_INDEX_THREADS,
                   help="threads for the POST /api/index after each restart "
                   "(default: %(default)s)")
    p.add_argument("--out", default=DEFAULT_OUT,
                   help="output directory (default: benchmarks/results)")
    p.set_defaults(func=cmd_throughput)
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except (OSError, RuntimeError, ImportError, subprocess.SubprocessError,
            http.client.HTTPException) as e:
        print("error: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
