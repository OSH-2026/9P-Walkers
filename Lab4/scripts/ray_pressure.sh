#!/usr/bin/env bash
# Ray concurrency pressure test for llama-server.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export SCRIPT_DIR

exec python3 - "$@" <<'PY'
import argparse
import csv
import json
import math
import os
import time
from collections import deque

try:
    import ray
except ImportError:
    ray = None


def load_prompts(path, limit):
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    prompts = [item["prompt"] for item in data]
    return prompts[:limit] if limit else prompts


def call_server(server, prompt, n_predict, timeout):
    import requests

    start = time.time()
    err = ""
    out_len = 0
    try:
        r = requests.post(
            f"{server}/completion",
            json={"prompt": prompt, "n_predict": n_predict},
            timeout=timeout,
        )
        r.raise_for_status()
        out_len = len(r.json().get("content", ""))
    except Exception as e:
        err = str(e)
    end = time.time()
    return {
        "server": server,
        "start": round(start, 3),
        "end": round(end, 3),
        "dur": round(end - start, 3),
        "out_len": out_len,
        "err": err,
    }


def percentile(values, p):
    if not values:
        return 0.0
    xs = sorted(values)
    idx = math.ceil(len(xs) * p / 100.0) - 1
    idx = min(max(idx, 0), len(xs) - 1)
    return xs[idx]


def run_concurrency(concurrency, servers, prompts, n_predict, timeout):
    remote_call = ray.remote(call_server)
    pending = deque(enumerate(prompts))
    inflight = []
    records = []
    t0 = time.time()

    def submit_one():
        i, prompt = pending.popleft()
        server = servers[i % len(servers)]
        ref = remote_call.remote(server, prompt, n_predict, timeout)
        inflight.append((i, server, ref))

    while pending and len(inflight) < concurrency:
        submit_one()

    while inflight:
        ready_refs, _ = ray.wait([item[2] for item in inflight], num_returns=1)
        ready = ready_refs[0]
        for pos, (i, server, ref) in enumerate(inflight):
            if ref == ready:
                record = ray.get(ref)
                record["idx"] = i
                records.append(record)
                del inflight[pos]
                break
        while pending and len(inflight) < concurrency:
            submit_one()

    wall = time.time() - t0
    ok = [r for r in records if not r["err"]]
    durs = [r["dur"] for r in ok]
    return records, {
        "concurrency": concurrency,
        "requests": len(records),
        "success": len(ok),
        "failures": len(records) - len(ok),
        "total_s": round(wall, 2),
        "avg_latency_s": round(sum(durs) / len(durs), 2) if durs else 0.0,
        "p95_latency_s": round(percentile(durs, 95), 2),
        "throughput_req_s": round(len(records) / wall, 2) if wall > 0 else 0.0,
    }


def main():
    script_dir = os.environ["SCRIPT_DIR"]
    ap = argparse.ArgumentParser(prog="ray_pressure.sh")
    ap.add_argument("--servers", nargs="+", required=True)
    ap.add_argument("--prompts", default=os.path.join(script_dir, "prompts.json"))
    ap.add_argument("--limit", type=int, default=8)
    ap.add_argument("--n-predict", type=int, default=8)
    ap.add_argument("--concurrency", default="1,2,4")
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument("--ray-address", default="local")
    ap.add_argument("--out-dir", default=os.path.join(script_dir, "results"))
    args = ap.parse_args()

    if ray is None:
        raise SystemExit("未安装 ray, 请先 pip install 'ray[default]'")

    os.makedirs(args.out_dir, exist_ok=True)
    prompts = load_prompts(args.prompts, args.limit)
    concurrencies = [int(x) for x in args.concurrency.split(",") if x.strip()]

    ray.init(address=args.ray_address)
    stamp = time.strftime("%Y%m%d_%H%M%S")
    all_records = []
    summaries = []

    for concurrency in concurrencies:
        records, summary = run_concurrency(
            concurrency, args.servers, prompts, args.n_predict, args.timeout
        )
        for record in records:
            record["concurrency"] = concurrency
        all_records.extend(records)
        summaries.append(summary)
        print(
            f"[c={concurrency}] n={summary['requests']} ok={summary['success']} "
            f"avg={summary['avg_latency_s']}s p95={summary['p95_latency_s']}s "
            f"thr={summary['throughput_req_s']} req/s fail={summary['failures']}"
        )

    detail = os.path.join(args.out_dir, f"ray_pressure_detail_{stamp}.csv")
    with open(detail, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "concurrency",
                "idx",
                "server",
                "start",
                "end",
                "dur",
                "out_len",
                "err",
            ],
        )
        writer.writeheader()
        writer.writerows(all_records)

    summary_path = os.path.join(args.out_dir, f"ray_pressure_summary_{stamp}.csv")
    with open(summary_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "concurrency",
                "requests",
                "success",
                "failures",
                "total_s",
                "avg_latency_s",
                "p95_latency_s",
                "throughput_req_s",
            ],
        )
        writer.writeheader()
        writer.writerows(summaries)

    print(f"明细: {detail}")
    print(f"汇总: {summary_path}")
    ray.shutdown()


if __name__ == "__main__":
    main()
PY
