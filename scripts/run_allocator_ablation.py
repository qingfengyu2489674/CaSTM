#!/usr/bin/env python3
"""Run the fixed system-allocator versus TierAlloc ablation.

The benchmark binary is built once per allocator mode and every measurement
is an independent process.  This runner intentionally contains exactly six
backend/workload/thread configurations; it is not a second full matrix.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import platform
import statistics
import subprocess
import sys
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, Iterable, List, Mapping, Sequence, Tuple


ALLOCATORS = ("system", "tier")
CONFIGS = (
    ("occ", 1, 50, "low"),
    ("ww", 1, 50, "low"),
    ("occ", 8, 50, "low"),
    ("ww", 8, 50, "low"),
    ("occ", 8, 50, "high"),
    ("ww", 20, 10, "high"),
)
RAW_FIELDS = (
    "allocator",
    "backend",
    "requested_threads",
    "threads",
    "read_ratio",
    "write_ratio",
    "contention",
    "run",
    "throughput_tps",
    "abort_rate",
    "p50_us",
    "p99_us",
    "attempts",
    "committed",
    "read_checksum",
    "final_checksum",
    "expected_checksum",
    "elapsed_s",
    "valid",
    "error",
)
SUMMARY_GROUP_FIELDS = ("allocator", "backend", "threads", "read_ratio", "contention")
SUMMARY_METRICS = (
    ("throughput_tps", "throughput_mean_tps", "throughput_stddev_tps"),
    ("abort_rate", "abort_rate_mean", "abort_rate_stddev"),
    ("p50_us", "p50_mean_us", "p50_stddev_us"),
    ("p99_us", "p99_mean_us", "p99_stddev_us"),
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--system-binary", type=Path, required=True)
    parser.add_argument("--tier-binary", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--warmup-ms", type=int, default=1000)
    parser.add_argument("--measure-ms", type=int, default=5000)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--ops-per-tx", type=int, default=8)
    parser.add_argument("--low-key-count", type=int, default=65536)
    parser.add_argument("--high-key-count", type=int, default=16)
    parser.add_argument("--seed", type=lambda value: int(value, 0), default=0xCA57BEEF)
    parser.add_argument("--max-attempts", type=int, default=1000000)
    parser.add_argument("--timeout-seconds", type=float)
    return parser.parse_args()


def run_command(command: Sequence[str], cwd: Path | None = None) -> str:
    completed = subprocess.run(
        list(command),
        cwd=str(cwd) if cwd else None,
        check=False,
        capture_output=True,
        text=True,
    )
    return completed.stdout.strip()


def parse_binary_row(stdout: str) -> Dict[str, str] | None:
    lines = [line for line in stdout.splitlines() if line.strip()]
    if not lines:
        return None
    try:
        rows = list(csv.DictReader(lines))
    except csv.Error:
        return None
    return rows[-1] if rows else None


def is_valid(row: Mapping[str, str]) -> bool:
    return row.get("valid", "0").strip().lower() in {"1", "true", "yes"}


def failed_row(
    allocator: str,
    backend: str,
    threads: int,
    read_ratio: int,
    contention: str,
    run: int,
    error: str,
) -> Dict[str, str]:
    return {
        "allocator": allocator,
        "backend": backend,
        "requested_threads": str(threads),
        "threads": str(threads),
        "read_ratio": str(read_ratio),
        "write_ratio": str(100 - read_ratio),
        "contention": contention,
        "run": str(run),
        "throughput_tps": "",
        "abort_rate": "",
        "p50_us": "",
        "p99_us": "",
        "attempts": "",
        "committed": "",
        "read_checksum": "",
        "final_checksum": "",
        "expected_checksum": "",
        "elapsed_s": "",
        "valid": "0",
        "error": error,
    }


def binary_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def cpu_model() -> str:
    try:
        with Path("/proc/cpuinfo").open("r", encoding="utf-8") as stream:
            for line in stream:
                if line.lower().startswith("model name") and ":" in line:
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def metadata_text(
    repo: Path,
    system_binary: Path,
    tier_binary: Path,
    args: argparse.Namespace,
    output_dir: Path,
) -> str:
    compiler_lines = run_command(("c++", "--version")).splitlines()
    parameters = {
        "allocators": list(ALLOCATORS),
        "configurations": [
            {
                "backend": backend,
                "threads": threads,
                "read_ratio": read_ratio,
                "contention": contention,
            }
            for backend, threads, read_ratio, contention in CONFIGS
        ],
        "repetitions": args.repetitions,
        "warmup_ms": args.warmup_ms,
        "measure_ms": args.measure_ms,
        "ops_per_tx": args.ops_per_tx,
        "low_key_count": args.low_key_count,
        "high_key_count": args.high_key_count,
        "seed": args.seed,
        "max_attempts": args.max_attempts,
    }
    lines = [
        "benchmark=CaSTM allocator ablation",
        f"git_commit={run_command(('git', '-C', str(repo), 'rev-parse', 'HEAD')) or 'unknown'}",
        f"result_dir={output_dir}",
        "build_type=Release",
        "optimization=-O3",
        "NDEBUG=1",
        "STM_WW_VERIFY_LOGIC_MODE=0",
        "STM_WW_TEST_HOOKS=0",
        "march_native=disabled (default; no -march=native)",
        f"compiler={(compiler_lines[0] if compiler_lines else 'unknown')}",
        f"cpu_model={cpu_model()}",
        f"logical_cpu_count={os.cpu_count() or 1}",
        f"os={platform.platform()}",
        f"system_allocator_binary={system_binary}",
        f"system_allocator_sha256={binary_sha256(system_binary)}",
        f"tier_allocator_binary={tier_binary}",
        f"tier_allocator_sha256={binary_sha256(tier_binary)}",
        "allocator_switch=STM_ALLOCATOR_MODE=system or tier at benchmark target configure time",
        "system_mode=Occ VersionNode/explicit transaction allocations use ::operator new/delete; Ww VersionNode/WriteRecord use ::operator new/delete",
        "tier_mode=Occ VersionNode/explicit transaction allocations use ThreadHeap; Ww VersionNode/WriteRecord use ThreadHeap",
        "native_mode=historical defaults: Occ TierAlloc, Ww system allocator",
        "excluded_from_switch=Ww TxDescriptor (alignas(64)); EBR GarbageNode metadata; std::vector buffers; benchmark TMVar containers; StripedLockTable",
        "tieralloc_config=kChunkSize=2MiB; kMaxAlloc=256KiB; kClassCount=104; kMaxCentralCacheSize=64",
        "contention_low=65536 keys",
        "contention_high=16 keys",
        "measurement_window=worker starts; in-flight logical transactions finish before join",
        "parameters=" + json.dumps(parameters, sort_keys=True),
    ]
    return "\n".join(lines) + "\n"


def summarize_rows(rows: Iterable[Mapping[str, str]]) -> List[Dict[str, str]]:
    grouped: Dict[Tuple[str, str, str, str, str], List[Mapping[str, str]]] = defaultdict(list)
    for row in rows:
        key = tuple(row.get(field, "") for field in SUMMARY_GROUP_FIELDS)
        grouped[key].append(row)

    output: List[Dict[str, str]] = []
    for key in sorted(grouped):
        group = grouped[key]
        valid_rows = [row for row in group if is_valid(row)]
        result = {field: value for field, value in zip(SUMMARY_GROUP_FIELDS, key)}
        result["write_ratio"] = str(100 - int(result["read_ratio"]))
        result["valid_runs"] = str(len(valid_rows))
        result["total_runs"] = str(len(group))
        for source, mean_name, stddev_name in SUMMARY_METRICS:
            values = []
            for row in valid_rows:
                try:
                    values.append(float(row[source]))
                except (KeyError, TypeError, ValueError):
                    pass
            if values:
                mean = statistics.mean(values)
                stddev = statistics.stdev(values) if len(values) > 1 else 0.0
                result[mean_name] = f"{mean:.6f}"
                result[stddev_name] = f"{stddev:.6f}"
            else:
                result[mean_name] = ""
                result[stddev_name] = ""
        output.append(result)
    return output


def write_summary(rows: Iterable[Mapping[str, str]], output_path: Path) -> None:
    summary_rows = summarize_rows(rows)
    fields = list(SUMMARY_GROUP_FIELDS) + [
        "write_ratio",
        "valid_runs",
        "total_runs",
    ]
    for _, mean_name, stddev_name in SUMMARY_METRICS:
        fields.extend((mean_name, stddev_name))
    with output_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(summary_rows)


def main() -> int:
    args = parse_args()
    if args.warmup_ms < 0 or args.measure_ms <= 0 or args.repetitions <= 0:
        raise SystemExit("warmup must be non-negative, measure and repetitions must be positive")
    if args.ops_per_tx <= 0 or args.low_key_count <= 0 or args.high_key_count <= 0:
        raise SystemExit("operation count and key counts must be positive")
    if args.max_attempts <= 0:
        raise SystemExit("max-attempts must be positive")

    repo = Path(__file__).resolve().parents[1]
    binaries = {"system": args.system_binary.resolve(), "tier": args.tier_binary.resolve()}
    for allocator, binary in binaries.items():
        if not binary.is_file():
            raise SystemExit(f"{allocator} benchmark binary does not exist: {binary}")

    if args.output_dir:
        output_dir = args.output_dir.resolve()
    else:
        timestamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S")
        output_dir = repo / "bench" / "results" / f"allocator-ablation-{timestamp}"
    output_dir.mkdir(parents=True, exist_ok=False)

    (output_dir / "metadata.txt").write_text(
        metadata_text(repo, binaries["system"], binaries["tier"], args, output_dir),
        encoding="utf-8",
    )

    timeout = args.timeout_seconds or max(
        30.0,
        args.warmup_ms / 1000.0 + args.measure_ms / 1000.0 + 30.0,
    )
    total = len(ALLOCATORS) * len(CONFIGS) * args.repetitions
    completed = 0
    had_invalid = False
    had_checksum_failure = False
    raw_rows: List[Dict[str, str]] = []

    with (output_dir / "raw.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=RAW_FIELDS)
        writer.writeheader()
        for allocator in ALLOCATORS:
            binary = binaries[allocator]
            for backend, threads, read_ratio, contention in CONFIGS:
                key_count = args.low_key_count if contention == "low" else args.high_key_count
                for run in range(1, args.repetitions + 1):
                    command = [
                        str(binary),
                        "--backend", backend,
                        "--threads", str(threads),
                        "--read-ratio", str(read_ratio),
                        "--contention", contention,
                        "--key-count", str(key_count),
                        "--ops-per-tx", str(args.ops_per_tx),
                        "--warmup-ms", str(args.warmup_ms),
                        "--measure-ms", str(args.measure_ms),
                        "--run", str(run),
                        "--seed", str(args.seed),
                        "--max-attempts", str(args.max_attempts),
                        "--csv",
                    ]
                    try:
                        completed_process = subprocess.run(
                            command,
                            cwd=str(repo),
                            check=False,
                            capture_output=True,
                            text=True,
                            timeout=timeout,
                        )
                        parsed = parse_binary_row(completed_process.stdout)
                        error = completed_process.stderr.strip()
                        if parsed is None:
                            row = failed_row(
                                allocator,
                                backend,
                                threads,
                                read_ratio,
                                contention,
                                run,
                                error or f"exit code {completed_process.returncode}",
                            )
                        else:
                            row = {field: parsed.get(field, "") for field in RAW_FIELDS}
                            row["allocator"] = allocator
                            row["backend"] = backend
                            row["requested_threads"] = str(threads)
                            row["threads"] = parsed.get("threads", str(threads))
                            row["read_ratio"] = str(read_ratio)
                            row["write_ratio"] = str(100 - read_ratio)
                            row["contention"] = contention
                            row["run"] = str(run)
                            row["error"] = error
                            if completed_process.returncode != 0:
                                row["error"] = row["error"] or f"exit code {completed_process.returncode}"

                        final_checksum = row.get("final_checksum", "")
                        expected_checksum = row.get("expected_checksum", "")
                        if final_checksum and expected_checksum and final_checksum != expected_checksum:
                            had_checksum_failure = True
                            row["valid"] = "0"
                            mismatch = f"checksum mismatch: final={final_checksum} expected={expected_checksum}"
                            row["error"] = f"{row.get('error', '')}; {mismatch}".lstrip("; ")
                        if not is_valid(row):
                            had_invalid = True
                        raw_rows.append(row)
                        writer.writerow(row)
                        stream.flush()
                    except subprocess.TimeoutExpired:
                        row = failed_row(
                            allocator,
                            backend,
                            threads,
                            read_ratio,
                            contention,
                            run,
                            f"timeout after {timeout:.1f}s",
                        )
                        had_invalid = True
                        raw_rows.append(row)
                        writer.writerow(row)
                        stream.flush()

                    completed += 1
                    print(
                        f"[{completed}/{total}] {allocator} {backend} threads={threads} "
                        f"read={read_ratio} contention={contention} run={run} "
                        f"valid={row.get('valid', '0')}",
                        file=sys.stderr,
                    )

    write_summary(raw_rows, output_dir / "summary.csv")
    valid_count = sum(1 for row in raw_rows if is_valid(row))
    print(f"raw={output_dir / 'raw.csv'}")
    print(f"summary={output_dir / 'summary.csv'}")
    print(f"metadata={output_dir / 'metadata.txt'}")
    print(f"valid_runs={valid_count}/{len(raw_rows)}")
    print(f"checksum_failures={int(had_checksum_failure)}")
    if had_checksum_failure:
        return 2
    return 1 if had_invalid else 0


if __name__ == "__main__":
    raise SystemExit(main())
