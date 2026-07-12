#!/usr/bin/env python3
"""Run comparable HTTP/1.1 workloads against the server's concurrency modes."""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import math
import os
import platform
import select
import signal
import socket
import statistics
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
REQUEST = b"GET /health HTTP/1.1\r\nHost: benchmark.local\r\n\r\n"
MODES = ("thread-per-connection", "thread-pool", "epoll")


def read_proc_stats(pid: int) -> tuple[int, int]:
    """Return process CPU ticks and resident memory in KiB from /proc."""
    stat_text = Path(f"/proc/{pid}/stat").read_text(encoding="ascii")
    fields = stat_text[stat_text.rfind(")") + 2 :].split()
    cpu_ticks = int(fields[11]) + int(fields[12])  # utime and stime
    status_text = Path(f"/proc/{pid}/status").read_text(encoding="ascii")
    rss_line = next(line for line in status_text.splitlines() if line.startswith("VmRSS:"))
    rss_kib = int(rss_line.split()[1])
    return cpu_ticks, rss_kib


def percentile(values: list[float], percentile_value: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = max(0, math.ceil(percentile_value * len(ordered)) - 1)
    return ordered[index]


def exchange(port: int, timeout_seconds: float) -> tuple[float, int, int, int]:
    start = time.perf_counter()
    with socket.create_connection(("127.0.0.1", port), timeout=timeout_seconds) as client:
        client.settimeout(timeout_seconds)
        client.sendall(REQUEST)
        response = bytearray()
        while True:
            chunk = client.recv(4096)
            if not chunk:
                break
            response.extend(chunk)
    elapsed_ms = (time.perf_counter() - start) * 1000.0
    header, separator, body = response.partition(b"\r\n\r\n")
    status_fields = header.split(b" ", 2)
    if not separator or len(status_fields) < 2 or not status_fields[1].isdigit():
        raise RuntimeError("server returned a malformed response")
    status_code = int(status_fields[1])
    return elapsed_ms, len(response), len(body), status_code


def run_load(
    port: int,
    concurrency: int,
    duration_seconds: float,
    pid: int,
    timeout_seconds: float,
    sample_process: bool,
) -> dict[str, Any]:
    barrier = threading.Barrier(concurrency + 1)
    worker_latencies: list[list[float]] = [[] for _ in range(concurrency)]
    worker_responses: list[int] = [0] * concurrency
    worker_body_sizes: list[int] = [0] * concurrency
    worker_errors: list[int] = [0] * concurrency
    worker_statuses: list[dict[str, int]] = [{} for _ in range(concurrency)]
    sampler_stop = threading.Event()
    cpu_samples: list[float] = []
    memory_samples: list[int] = []

    def sample_usage() -> None:
        try:
            last_ticks, last_rss = read_proc_stats(pid)
        except (FileNotFoundError, ProcessLookupError):
            return
        last_time = time.perf_counter()
        memory_samples.append(last_rss)
        ticks_per_second = float(os.sysconf("SC_CLK_TCK"))
        logical_cpus = max(1, os.cpu_count() or 1)
        while not sampler_stop.wait(0.1):
            try:
                ticks, rss = read_proc_stats(pid)
            except (FileNotFoundError, ProcessLookupError):
                break
            current_time = time.perf_counter()
            elapsed = current_time - last_time
            if elapsed > 0:
                cpu_samples.append(
                    max(0.0, (ticks - last_ticks) / ticks_per_second / elapsed / logical_cpus * 100.0)
                )
            memory_samples.append(rss)
            last_ticks, last_rss, last_time = ticks, rss, current_time

    def worker(index: int) -> None:
        barrier.wait()
        deadline = start_time[0] + duration_seconds
        while time.perf_counter() < deadline:
            try:
                latency, response_bytes, body_bytes, status_code = exchange(port, timeout_seconds)
                if status_code != 200:
                    worker_errors[index] += 1
                    status_key = str(status_code)
                    worker_statuses[index][status_key] = worker_statuses[index].get(status_key, 0) + 1
                    continue
                worker_latencies[index].append(latency)
                worker_responses[index] += response_bytes
                worker_body_sizes[index] = body_bytes
            except (OSError, RuntimeError, TimeoutError):
                worker_errors[index] += 1
                worker_statuses[index]["transport_or_protocol_error"] = (
                    worker_statuses[index].get("transport_or_protocol_error", 0) + 1
                )

    start_time = [0.0]
    sampler_thread: threading.Thread | None = None
    if sample_process:
        sampler_thread = threading.Thread(target=sample_usage, daemon=True)
        sampler_thread.start()

    start = time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as executor:
        futures = [executor.submit(worker, index) for index in range(concurrency)]
        start_time[0] = time.perf_counter()
        start = start_time[0]
        barrier.wait()
        for future in futures:
            future.result()
    end = time.perf_counter()

    sampler_stop.set()
    if sampler_thread is not None:
        sampler_thread.join()

    latencies = [latency for samples in worker_latencies for latency in samples]
    completed = len(latencies)
    elapsed = max(end - start, 1e-9)
    responses = sum(worker_responses)
    status_counts: dict[str, int] = {}
    for statuses in worker_statuses:
        for status, count in statuses.items():
            status_counts[status] = status_counts.get(status, 0) + count
    return {
        "elapsed_seconds": elapsed,
        "completed_requests": completed,
        "failed_requests": sum(worker_errors),
        "non_200_or_transport_errors": status_counts,
        "requests_per_second": completed / elapsed,
        "latency_ms_median": statistics.median(latencies) if latencies else 0.0,
        "latency_ms_p95": percentile(latencies, 0.95),
        "latency_ms_p99": percentile(latencies, 0.99),
        "cpu_percent_of_machine": statistics.mean(cpu_samples) if cpu_samples else 0.0,
        "cpu_percent_peak_sample": max(cpu_samples, default=0.0),
        "rss_mib_average": statistics.mean(memory_samples) / 1024.0 if memory_samples else 0.0,
        "rss_mib_peak_sample": max(memory_samples, default=0) / 1024.0,
        "response_bytes_per_request": responses / completed if completed else 0.0,
        "response_body_bytes": max(worker_body_sizes, default=0),
        "request_bytes": len(REQUEST),
        "concurrency": concurrency,
        "target_duration_seconds": duration_seconds,
    }


def start_server(binary: Path, mode: str, workers: int, timeout_seconds: float) -> tuple[subprocess.Popen[str], int]:
    command = [str(binary), "--mode", mode, "--bind", "127.0.0.1", "--port", "0"]
    if mode == "thread-pool":
        command.extend(["--workers", str(workers)])
    process = subprocess.Popen(
        command,
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=None,
        text=True,
        bufsize=1,
    )
    assert process.stdout is not None
    ready, _, _ = select.select([process.stdout], [], [], timeout_seconds)
    if not ready:
        process.terminate()
        process.wait(timeout=timeout_seconds)
        raise RuntimeError(f"server did not become ready: {' '.join(command)}")
    line = process.stdout.readline().strip()
    if not line.startswith("READY 127.0.0.1:"):
        process.terminate()
        process.wait(timeout=timeout_seconds)
        raise RuntimeError(f"unexpected server startup output: {line!r}")
    return process, int(line.rsplit(":", 1)[1])


def stop_server(process: subprocess.Popen[str], timeout_seconds: float) -> None:
    if process.poll() is None:
        process.send_signal(signal.SIGTERM)
    try:
        process.wait(timeout=timeout_seconds)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
    if process.returncode != 0:
        raise RuntimeError(f"server exited with status {process.returncode}")


def compiler_description() -> str:
    compiler = subprocess.run(["g++", "--version"], capture_output=True, text=True, check=False)
    return compiler.stdout.splitlines()[0] if compiler.stdout else "unavailable"


def machine_description() -> tuple[str, int]:
    cpuinfo = Path("/proc/cpuinfo").read_text(encoding="utf-8", errors="replace")
    cpu_model = next(
        (line.split(":", 1)[1].strip() for line in cpuinfo.splitlines()
         if line.startswith("model name")),
        "unavailable",
    )
    meminfo = Path("/proc/meminfo").read_text(encoding="ascii")
    memory_kib = int(next(line.split()[1] for line in meminfo.splitlines()
                          if line.startswith("MemTotal:")))
    return cpu_model, memory_kib // 1024


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build/release/src/httpd")
    parser.add_argument("--modes", nargs="+", choices=MODES, default=list(MODES))
    parser.add_argument("--concurrency", type=int, default=50)
    parser.add_argument("--workers", type=int, default=4, help="fixed-pool worker count")
    parser.add_argument("--warmup-seconds", type=float, default=2.0)
    parser.add_argument("--duration-seconds", type=float, default=5.0)
    parser.add_argument("--trials", type=int, default=3)
    parser.add_argument("--timeout-seconds", type=float, default=3.0)
    parser.add_argument(
        "--output",
        type=Path,
        default=ROOT / "benchmark/results/latest.json",
        help="JSON result path",
    )
    args = parser.parse_args()

    if not sys.platform.startswith("linux"):
        parser.error("this harness uses Linux /proc and must run under Ubuntu/WSL2")
    if not args.binary.is_file():
        parser.error(f"server binary does not exist: {args.binary}")
    if not 1 <= args.concurrency <= 1024:
        parser.error("--concurrency must be from 1 to 1024")
    if not 1 <= args.workers <= 64:
        parser.error("--workers must be from 1 to 64")
    if args.warmup_seconds < 0 or not 0.1 <= args.duration_seconds <= 600:
        parser.error("durations must be nonnegative; measured duration must be 0.1 to 600 seconds")
    if not 1 <= args.trials <= 20 or args.timeout_seconds <= 0:
        parser.error("--trials must be 1 to 20 and --timeout-seconds must be positive")

    binary = args.binary.resolve()
    cpu_model, memory_mib = machine_description()
    result: dict[str, Any] = {
        "schema_version": 1,
        "recorded_at_utc": datetime.now(timezone.utc).isoformat(),
        "machine": {
            "hostname": socket.gethostname(),
            "platform": platform.platform(),
            "machine": platform.machine(),
            "logical_cpu_count": os.cpu_count(),
            "cpu_model": cpu_model,
            "memory_mib": memory_mib,
            "compiler": compiler_description(),
            "build_type": "Release (-O2 -DNDEBUG; release CMake preset)",
            "server_binary": str(binary),
        },
        "workload": {
            "endpoint": "/health",
            "request": REQUEST.decode("ascii"),
            "warmup_seconds": args.warmup_seconds,
            "measured_duration_seconds": args.duration_seconds,
            "trials": args.trials,
            "concurrency": args.concurrency,
            "thread_pool_workers": args.workers,
            "client_timeout_seconds": args.timeout_seconds,
            "connection_policy": "one new TCP connection per HTTP request; server closes each response",
        },
        "modes": {},
    }

    for mode in args.modes:
        print(f"Benchmarking {mode} ...", flush=True)
        process, port = start_server(binary, mode, args.workers, args.timeout_seconds)
        try:
            if args.warmup_seconds > 0:
                warmup = run_load(port, args.concurrency, args.warmup_seconds, process.pid,
                                  args.timeout_seconds, sample_process=False)
                if not warmup["completed_requests"]:
                    raise RuntimeError(f"warm-up failed for {mode}: {warmup}")
            else:
                warmup = None
            trials = [
                run_load(port, args.concurrency, args.duration_seconds, process.pid,
                         args.timeout_seconds, sample_process=True)
                for _ in range(args.trials)
            ]
            for trial_number, trial in enumerate(trials, 1):
                if not trial["completed_requests"]:
                    raise RuntimeError(f"trial {trial_number} failed for {mode}: {trial}")
        finally:
            stop_server(process, args.timeout_seconds)
        result["modes"][mode] = {
            "warmup": warmup,
            "trials": trials,
            "median_requests_per_second": statistics.median(
                trial["requests_per_second"] for trial in trials
            ),
            "median_latency_ms": statistics.median(trial["latency_ms_median"] for trial in trials),
            "median_latency_ms_p95": statistics.median(trial["latency_ms_p95"] for trial in trials),
            "median_latency_ms_p99": statistics.median(trial["latency_ms_p99"] for trial in trials),
            "median_cpu_percent_of_machine": statistics.median(
                trial["cpu_percent_of_machine"] for trial in trials
            ),
            "peak_rss_mib": max(trial["rss_mib_peak_sample"] for trial in trials),
        }
        print(f"  median requests/s: {result['modes'][mode]['median_requests_per_second']:.1f}",
              flush=True)

    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"Results written to {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
