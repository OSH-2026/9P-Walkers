#!/usr/bin/env bash
# Ray retry test for llama-server failure injection.

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


def main():
    script_dir = os.environ["SCRIPT_DIR"]
    ap = argparse.ArgumentParser(prog="ray_retry.sh")
    ap.add_argument("--servers", nargs="+", required=True)
    ap.add_argument("--prompts", default=os.path.join(script_dir, "prompts.json"))
    ap.add_argument("--limit", type=int, default=8)
    ap.add_argument("--n-predict", type=int, default=8)
    ap.add_argument("--timeout", type=int, default=30)
    ap.add_argument("--ray-address", default="local")
    ap.add_argument("--out-dir", default=os.path.join(script_dir, "results"))
    args = ap.parse_args()

    if ray is None:
        raise SystemExit("未安装 ray, 请先 pip install 'ray[default]'")

    os.makedirs(args.out_dir, exist_ok=True)
    prompts = load_prompts(args.prompts, args.limit)
    ray.init(address=args.ray_address)
    remote_call = ray.remote(call_server)
    stamp = time.strftime("%Y%m%d_%H%M%S")
    rows = []

    t0 = time.time()
    for idx, prompt in enumerate(prompts):
        attempts = 0
        final = None
        for offset in range(len(args.servers)):
            server = args.servers[(idx + offset) % len(args.servers)]
            attempts += 1
            result = ray.get(remote_call.remote(server, prompt, args.n_predict, args.timeout))
            row = {
                "idx": idx,
                "attempt": attempts,
                "server": server,
                "success": 0 if result["err"] else 1,
                "dur": result["dur"],
                "out_len": result["out_len"],
                "err": result["err"],
            }
            rows.append(row)
            if not result["err"]:
                final = result
                break
        print(
            f"[request {idx}] attempts={attempts} success={1 if final else 0} "
            f"server={final['server'] if final else 'n/a'}"
        )

    wall = time.time() - t0
    final_success = len({row["idx"] for row in rows if row["success"]})
    retries = sum(1 for row in rows if row["attempt"] > 1)
    failed_attempts = sum(1 for row in rows if not row["success"])
    summary = {
        "requests": len(prompts),
        "attempts": len(rows),
        "failed_attempts": failed_attempts,
        "retry_attempts": retries,
        "success": final_success,
        "success_rate": round(final_success / len(prompts), 4) if prompts else 0.0,
        "total_s": round(wall, 2),
    }

    detail = os.path.join(args.out_dir, f"ray_retry_detail_{stamp}.csv")
    with open(detail, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(
            f, fieldnames=["idx", "attempt", "server", "success", "dur", "out_len", "err"]
        )
        writer.writeheader()
        writer.writerows(rows)

    summary_path = os.path.join(args.out_dir, f"ray_retry_summary_{stamp}.csv")
    with open(summary_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=list(summary.keys()))
        writer.writeheader()
        writer.writerow(summary)

    print(
        f"请求={summary['requests']} 尝试={summary['attempts']} "
        f"失败尝试={summary['failed_attempts']} 成功率={summary['success_rate']:.2%}"
    )
    print(f"明细: {detail}")
    print(f"汇总: {summary_path}")
    ray.shutdown()


if __name__ == "__main__":
    main()
PY
