#!/usr/bin/env python3
"""
模块职责：流式 /chat 的 TTFT 采样脚本（Week 4.2 命令固化用）。
对外暴露：命令行参数（url/concurrency/requests/timeout_ms），输出 TTFT 统计。

注意：
1) 该脚本用于压测命令固化，不负责 4.3 的结果对比结论。
2) 仅依赖 Python 标准库，避免环境差异导致“命令不可复现”。
"""

from __future__ import annotations

import argparse
import json
import queue
import statistics
import threading
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import List


@dataclass
class SampleResult:
    ok: bool
    ttft_ms: float
    status_code: int
    error: str


def percentile(values: List[float], p: float) -> float:
    if not values:
        return 0.0
    if len(values) == 1:
        return values[0]
    idx = (len(values) - 1) * p
    low = int(idx)
    high = min(low + 1, len(values) - 1)
    frac = idx - low
    return values[low] * (1.0 - frac) + values[high] * frac


def one_request(url: str, timeout_sec: float, request_id: str) -> SampleResult:
    payload = {
        "request_id": request_id,
        "message": "ttft benchmark",
        "stream": True,
    }
    req = urllib.request.Request(
        url=url,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )

    start = time.perf_counter()
    try:
        with urllib.request.urlopen(req, timeout=timeout_sec) as resp:
            status_code = getattr(resp, "status", 0)
            if status_code != 200:
                return SampleResult(False, 0.0, status_code, f"unexpected_status_{status_code}")

            # 关键分支说明：TTFT 以首个 data: 行到达时间为准，不等待整段流结束。
            while True:
                line = resp.readline()
                if not line:
                    return SampleResult(False, 0.0, status_code, "stream_closed_before_first_data")
                if line.startswith(b"data:"):
                    ttft_ms = (time.perf_counter() - start) * 1000.0
                    return SampleResult(True, ttft_ms, status_code, "")
    except urllib.error.HTTPError as exc:
        return SampleResult(False, 0.0, exc.code, f"http_error_{exc.code}")
    except Exception as exc:  # noqa: BLE001
        return SampleResult(False, 0.0, 0, str(exc))


def worker(
    url: str,
    timeout_sec: float,
    req_ids: "queue.Queue[str]",
    out: List[SampleResult],
    lock: threading.Lock,
) -> None:
    while True:
        try:
            req_id = req_ids.get_nowait()
        except queue.Empty:
            return
        result = one_request(url, timeout_sec, req_id)
        with lock:
            out.append(result)
        req_ids.task_done()


def main() -> int:
    parser = argparse.ArgumentParser(description="stream TTFT benchmark")
    parser.add_argument("--url", required=True, help="target url, e.g. http://127.0.0.1:8080/chat")
    parser.add_argument("--concurrency", type=int, default=100)
    parser.add_argument("--requests", type=int, default=200)
    parser.add_argument("--timeout-ms", type=int, default=30000)
    parser.add_argument("--request-id-prefix", default="ttft")
    args = parser.parse_args()

    if args.concurrency <= 0 or args.requests <= 0:
        print("ERROR: concurrency/requests 必须 > 0")
        return 1

    timeout_sec = args.timeout_ms / 1000.0
    req_queue: "queue.Queue[str]" = queue.Queue()
    for i in range(args.requests):
        req_queue.put(f"{args.request_id_prefix}-{i}")

    results: List[SampleResult] = []
    lock = threading.Lock()
    threads = []

    begin = time.perf_counter()
    for _ in range(args.concurrency):
        t = threading.Thread(target=worker, args=(args.url, timeout_sec, req_queue, results, lock), daemon=True)
        t.start()
        threads.append(t)

    for t in threads:
        t.join()
    elapsed_s = time.perf_counter() - begin

    oks = [r.ttft_ms for r in results if r.ok]
    fails = [r for r in results if not r.ok]
    oks.sort()

    print("======================================================")
    print("流式 TTFT 采样结果")
    print(f"URL            : {args.url}")
    print(f"并发           : {args.concurrency}")
    print(f"总请求数       : {args.requests}")
    print(f"成功/失败      : {len(oks)}/{len(fails)}")
    print(f"总耗时(秒)     : {elapsed_s:.2f}")
    if oks:
        print(f"TTFT avg(ms)   : {statistics.mean(oks):.2f}")
        print(f"TTFT p50(ms)   : {percentile(oks, 0.50):.2f}")
        print(f"TTFT p95(ms)   : {percentile(oks, 0.95):.2f}")
        print(f"TTFT p99(ms)   : {percentile(oks, 0.99):.2f}")
    else:
        print("TTFT avg/p50/p95/p99: N/A")
    print("======================================================")

    if fails:
        # 易踩坑：异常样本只打印前 5 条，避免高并发时刷屏掩盖关键信息。
        print("失败样本（最多 5 条）：")
        for item in fails[:5]:
            print(f"  status={item.status_code} error={item.error}")
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
