#!/usr/bin/env bash
# ray_dispatch.sh - dispatch prompts to llama-server instances with Ray.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export SCRIPT_DIR

exec python3 - "$@" <<'PY'
import argparse
import csv
import json
import os
import time

try:
    import ray
except ImportError:
    ray = None


def load_prompts(path):
    with open(path, "r", encoding="utf-8") as f:
        return [item["prompt"] for item in json.load(f)]


def call_server(server, prompt, n_predict):
    """Send one synchronous /completion request and return timing metrics."""
    import requests

    t0 = time.time()
    err = None
    out = ""
    try:
        r = requests.post(
            f"{server}/completion",
            json={"prompt": prompt, "n_predict": n_predict},
            timeout=600,
        )
        r.raise_for_status()
        out = r.json().get("content", "")
    except Exception as e:
        err = str(e)
    t1 = time.time()
    return {
        "server": server,
        "start": round(t0, 3),
        "end": round(t1, 3),
        "dur": round(t1 - t0, 3),
        "out_len": len(out),
        "err": err,
    }


def run_serial(servers, prompts, n_predict):
    res = []
    for i, prompt in enumerate(prompts):
        res.append(call_server(servers[i % len(servers)], prompt, n_predict))
    return res


def run_parallel(servers, prompts, n_predict):
    """Use Ray tasks; one server means single-node parallel, many means multi-node."""
    remote_call = ray.remote(call_server)
    futures = [
        remote_call.remote(servers[i % len(servers)], prompt, n_predict)
        for i, prompt in enumerate(prompts)
    ]
    return ray.get(futures)


def summarize(mode, results, wall):
    n = len(results)
    ok = [r for r in results if not r["err"]]
    avg = sum(r["dur"] for r in ok) / len(ok) if ok else 0
    throughput = n / wall if wall > 0 else 0
    print(f"\n===== [{mode}] 汇总 =====")
    print(f"请求数      : {n}  (成功 {len(ok)}, 失败 {n - len(ok)})")
    print(f"总耗时(s)   : {wall:.2f}")
    print(f"平均延迟(s) : {avg:.2f}")
    print(f"吞吐(req/s) : {throughput:.2f}")
    return {
        "mode": mode,
        "n": n,
        "ok": len(ok),
        "total_s": round(wall, 2),
        "avg_latency_s": round(avg, 2),
        "throughput_req_s": round(throughput, 2),
    }


def main():
    script_dir = os.environ["SCRIPT_DIR"]

    ap = argparse.ArgumentParser(prog="ray_dispatch.sh")
    ap.add_argument(
        "--servers",
        nargs="+",
        required=True,
        help="llama-server 地址列表, 例: http://127.0.0.1:8080 http://127.0.0.1:8081",
    )
    ap.add_argument("--prompts", default=os.path.join(script_dir, "prompts.json"))
    ap.add_argument("--n-predict", type=int, default=128)
    ap.add_argument("--mode", choices=["serial", "single", "multi", "all"], default="all")
    ap.add_argument(
        "--ray-address",
        default="auto",
        help="Ray 集群地址, 默认 auto; 单机自测可用 local",
    )
    ap.add_argument("--out-dir", default=os.path.join(script_dir, "results"))
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)
    prompts = load_prompts(args.prompts)
    print(f"加载 {len(prompts)} 个 prompt; servers={args.servers}")

    modes = ["serial", "single", "multi"] if args.mode == "all" else [args.mode]
    need_ray = any(m in ("single", "multi") for m in modes)
    if need_ray:
        if ray is None:
            raise SystemExit("未安装 ray, 请先 pip install 'ray[default]'")
        ray.init(address=args.ray_address)

    stamp = time.strftime("%Y%m%d_%H%M%S")
    all_records = []
    summaries = []
    for mode in modes:
        t0 = time.time()
        if mode == "serial":
            res = run_serial(args.servers, prompts, args.n_predict)
        elif mode == "single":
            res = run_parallel(args.servers[:1], prompts, args.n_predict)
        else:
            res = run_parallel(args.servers, prompts, args.n_predict)
        wall = time.time() - t0
        for r in res:
            r["mode"] = mode
        all_records.extend(res)
        summaries.append(summarize(mode, res, wall))

    detail = os.path.join(args.out_dir, f"ray_detail_{stamp}.csv")
    with open(detail, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(
            f, fieldnames=["mode", "server", "start", "end", "dur", "out_len", "err"]
        )
        w.writeheader()
        w.writerows(all_records)

    summ = os.path.join(args.out_dir, f"ray_summary_{stamp}.csv")
    with open(summ, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(
            f,
            fieldnames=[
                "mode",
                "n",
                "ok",
                "total_s",
                "avg_latency_s",
                "throughput_req_s",
            ],
        )
        w.writeheader()
        w.writerows(summaries)

    print(f"\n明细: {detail}\n汇总: {summ}")
    if need_ray:
        ray.shutdown()


if __name__ == "__main__":
    main()
PY
