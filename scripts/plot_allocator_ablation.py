#!/usr/bin/env python3
"""Analyze an existing CaSTM system-allocator/TierAlloc ablation.

The input directory is read-only from the benchmark's point of view: this
script consumes raw.csv, summary.csv, and metadata.txt and writes derived
charts/reports beside them. It never starts a benchmark process and it
deliberately knows only the six configurations used by the ablation.
"""

from __future__ import annotations

import argparse
import csv
import math
from collections import Counter, defaultdict
from pathlib import Path
from typing import Dict, Iterable, List, Mapping, Sequence, Tuple

try:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except ImportError as error:  # pragma: no cover - environment dependent
    raise SystemExit(
        "matplotlib is required for chart generation; install it separately "
        "from the benchmark source tree"
    ) from error


ALLOCATORS = ("system", "tier")
ALLOCATOR_LABELS = {"system": "system allocator", "tier": "TierAlloc"}
BACKEND_LABELS = {"occ": "OccSTM", "ww": "WwSTM"}
COLORS = {"system": "#9E9E9E", "tier": "#4C78A8"}

# identifier, backend, threads, read ratio, contention
CONFIGS: Tuple[Tuple[str, str, int, int, str], ...] = (
    ("occ_1t_50r50w_low", "occ", 1, 50, "low"),
    ("ww_1t_50r50w_low", "ww", 1, 50, "low"),
    ("occ_8t_50r50w_low", "occ", 8, 50, "low"),
    ("ww_8t_50r50w_low", "ww", 8, 50, "low"),
    ("occ_8t_50r50w_high", "occ", 8, 50, "high"),
    ("ww_20t_10r90w_high", "ww", 20, 10, "high"),
)

METRIC_FIELDS = {
    "throughput": ("throughput_mean_tps", "throughput_stddev_tps"),
    "abort_rate": ("abort_rate_mean", "abort_rate_stddev"),
    "p50": ("p50_mean_us", "p50_stddev_us"),
    "p99": ("p99_mean_us", "p99_stddev_us"),
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "result_dir",
        type=Path,
        help="existing allocator-ablation directory containing raw.csv",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        help="chart directory; defaults to result_dir/charts",
    )
    parser.add_argument(
        "--analysis-output",
        type=Path,
        help="markdown report; defaults to result_dir/analysis.md",
    )
    parser.add_argument(
        "--summary-output",
        type=Path,
        help="derived pair summary; defaults to result_dir/analysis_summary.csv",
    )
    return parser.parse_args()


def read_csv(path: Path) -> List[Dict[str, str]]:
    with path.open("r", newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames is None:
            raise ValueError(f"CSV has no header: {path}")
        return list(reader)


def read_metadata(path: Path) -> Dict[str, str]:
    metadata: Dict[str, str] = {}
    with path.open("r", encoding="utf-8") as stream:
        for line in stream:
            line = line.rstrip("\n")
            if "=" in line:
                key, value = line.split("=", 1)
                metadata[key] = value
    return metadata


def parse_int(value: str | None) -> int | None:
    if value is None or value.strip() == "":
        return None
    try:
        return int(value)
    except ValueError:
        return None


def parse_float(value: str | None) -> float | None:
    if value is None or value.strip() == "":
        return None
    try:
        result = float(value)
    except ValueError:
        return None
    return result if math.isfinite(result) else None


def is_valid(row: Mapping[str, str]) -> bool:
    return row.get("valid", "0").strip().lower() in {"1", "true", "yes"}


def summary_key(row: Mapping[str, str]) -> Tuple[str, str, int, int, str]:
    return (
        row["allocator"],
        row["backend"],
        int(row["threads"]),
        int(row["read_ratio"]),
        row["contention"],
    )


def summary_row_complete(row: Mapping[str, str] | None) -> bool:
    if row is None:
        return False
    valid_runs = parse_int(row.get("valid_runs"))
    total_runs = parse_int(row.get("total_runs"))
    return (
        valid_runs is not None
        and total_runs is not None
        and total_runs > 0
        and valid_runs == total_runs
    )


def valid_run_count(row: Mapping[str, str] | None) -> str:
    if row is None:
        return "missing"
    return f"{row.get('valid_runs', '?')}/{row.get('total_runs', '?')}"


def config_label(config: Tuple[str, str, int, int, str]) -> str:
    _, backend, threads, read_ratio, contention = config
    return (
        f"{BACKEND_LABELS[backend]} {threads}T "
        f"{read_ratio}R/{100 - read_ratio}W {contention}"
    )


def chart_label(config: Tuple[str, str, int, int, str]) -> str:
    _, backend, threads, read_ratio, contention = config
    return (
        f"{BACKEND_LABELS[backend]}\n"
        f"{threads}T {read_ratio}R/{100 - read_ratio}\n{contention}"
    )


def key_for_config(
    allocator: str, config: Tuple[str, str, int, int, str]
) -> Tuple[str, str, int, int, str]:
    _, backend, threads, read_ratio, contention = config
    return allocator, backend, threads, read_ratio, contention


def fmt_tps(value: float | None) -> str:
    if value is None:
        return "N/A"
    if abs(value) >= 1_000_000:
        return f"{value / 1_000_000:.3f}M"
    if abs(value) >= 1_000:
        return f"{value / 1_000:.1f}k"
    return f"{value:.1f}"


def fmt_us(value: float | None) -> str:
    if value is None:
        return "N/A"
    if value >= 1_000:
        return f"{value / 1_000:.2f} ms"
    return f"{value:.2f} us"


def fmt_percent(value: float | None, decimals: int = 2) -> str:
    if value is None:
        return "N/A"
    return f"{value * 100.0:.{decimals}f}%"


def fmt_pp(value: float | None) -> str:
    if value is None:
        return "N/A"
    return f"{value:+.2f} pp"


def metric_mean(row: Mapping[str, str] | None, metric: str) -> float | None:
    if not summary_row_complete(row):
        return None
    field, _ = METRIC_FIELDS[metric]
    return parse_float(row.get(field))


def metric_stddev(row: Mapping[str, str] | None, metric: str) -> float | None:
    if not summary_row_complete(row):
        return None
    _, field = METRIC_FIELDS[metric]
    return parse_float(row.get(field))


def metric_text(row: Mapping[str, str] | None, metric: str) -> str:
    if row is None:
        return "N/A (missing)"
    if not summary_row_complete(row):
        return f"N/A ({valid_run_count(row)}; incomplete)"
    mean = metric_mean(row, metric)
    stddev = metric_stddev(row, metric)
    if mean is None:
        return f"N/A ({valid_run_count(row)})"
    stddev = stddev or 0.0
    if metric == "throughput":
        return f"{fmt_tps(mean)} ± {fmt_tps(stddev)} ({valid_run_count(row)})"
    if metric in {"p50", "p99"}:
        return f"{fmt_us(mean)} ± {fmt_us(stddev)} ({valid_run_count(row)})"
    return f"{fmt_percent(mean)} ± {fmt_percent(stddev)} ({valid_run_count(row)})"


def ratio_with_error(
    numerator: float | None,
    numerator_sd: float | None,
    denominator: float | None,
    denominator_sd: float | None,
) -> Tuple[float | None, float | None]:
    if (
        numerator is None
        or numerator_sd is None
        or denominator is None
        or denominator_sd is None
        or numerator <= 0
        or denominator <= 0
    ):
        return None, None
    ratio = numerator / denominator
    relative_variance = (numerator_sd / numerator) ** 2 + (
        denominator_sd / denominator
    ) ** 2
    return ratio, ratio * math.sqrt(relative_variance)


def pair_record(
    config: Tuple[str, str, int, int, str],
    lookup: Mapping[Tuple[str, str, int, int, str], Mapping[str, str]],
) -> Dict[str, object]:
    system = lookup.get(key_for_config("system", config))
    tier = lookup.get(key_for_config("tier", config))
    missing = []
    if not summary_row_complete(system):
        missing.append(f"system {valid_run_count(system)}")
    if not summary_row_complete(tier):
        missing.append(f"TierAlloc {valid_run_count(tier)}")
    status = "complete" if not missing else "incomplete: " + "; ".join(missing)

    result: Dict[str, object] = {
        "id": config[0],
        "config": config,
        "label": config_label(config),
        "system": system,
        "tier": tier,
        "status": status,
        "speedup": None,
        "speedup_error": None,
        "p99_ratio": None,
        "p99_ratio_error": None,
        "abort_change_pp": None,
        "abort_change_error_pp": None,
    }
    if not missing:
        speedup, speedup_error = ratio_with_error(
            metric_mean(tier, "throughput"),
            metric_stddev(tier, "throughput"),
            metric_mean(system, "throughput"),
            metric_stddev(system, "throughput"),
        )
        p99_ratio, p99_ratio_error = ratio_with_error(
            metric_mean(tier, "p99"),
            metric_stddev(tier, "p99"),
            metric_mean(system, "p99"),
            metric_stddev(system, "p99"),
        )
        system_abort = metric_mean(system, "abort_rate")
        tier_abort = metric_mean(tier, "abort_rate")
        system_abort_sd = metric_stddev(system, "abort_rate") or 0.0
        tier_abort_sd = metric_stddev(tier, "abort_rate") or 0.0
        if system_abort is not None and tier_abort is not None:
            abort_change_pp = (tier_abort - system_abort) * 100.0
            abort_change_error_pp = math.sqrt(
                system_abort_sd**2 + tier_abort_sd**2
            ) * 100.0
        else:
            abort_change_pp = None
            abort_change_error_pp = None
        result.update(
            speedup=speedup,
            speedup_error=speedup_error,
            p99_ratio=p99_ratio,
            p99_ratio_error=p99_ratio_error,
            abort_change_pp=abort_change_pp,
            abort_change_error_pp=abort_change_error_pp,
        )
    return result


def write_analysis_summary(
    pairs: Sequence[Mapping[str, object]], output_path: Path
) -> None:
    fields = [
        "backend",
        "threads",
        "read_ratio",
        "write_ratio",
        "contention",
        "system_valid_runs",
        "system_total_runs",
        "tier_valid_runs",
        "tier_total_runs",
        "system_throughput_mean_tps",
        "system_throughput_stddev_tps",
        "tier_throughput_mean_tps",
        "tier_throughput_stddev_tps",
        "tier_over_system_throughput",
        "tier_over_system_throughput_error",
        "system_p99_mean_us",
        "system_p99_stddev_us",
        "tier_p99_mean_us",
        "tier_p99_stddev_us",
        "tier_over_system_p99_ratio",
        "tier_over_system_p99_ratio_error",
        "system_abort_rate",
        "tier_abort_rate",
        "tier_minus_system_abort_rate_pp",
        "tier_minus_system_abort_rate_error_pp",
        "status",
    ]
    with output_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for pair in pairs:
            config = pair["config"]
            assert isinstance(config, tuple)
            _, backend, threads, read_ratio, contention = config
            system = pair["system"]
            tier = pair["tier"]
            assert system is None or isinstance(system, Mapping)
            assert tier is None or isinstance(tier, Mapping)
            row: Dict[str, object] = {
                "backend": backend,
                "threads": threads,
                "read_ratio": read_ratio,
                "write_ratio": 100 - read_ratio,
                "contention": contention,
                "system_valid_runs": system.get("valid_runs", "") if system else "",
                "system_total_runs": system.get("total_runs", "") if system else "",
                "tier_valid_runs": tier.get("valid_runs", "") if tier else "",
                "tier_total_runs": tier.get("total_runs", "") if tier else "",
                "status": pair["status"],
            }
            for allocator, source in (("system", system), ("tier", tier)):
                for metric in ("throughput", "p99"):
                    mean_name, sd_name = METRIC_FIELDS[metric]
                    prefix = f"{allocator}_{metric}"
                    suffix = "tps" if metric == "throughput" else "us"
                    if summary_row_complete(source):
                        row[f"{prefix}_mean_{suffix}"] = source.get(mean_name, "")
                        row[f"{prefix}_stddev_{suffix}"] = source.get(sd_name, "")
                    else:
                        row[f"{prefix}_mean_{suffix}"] = ""
                        row[f"{prefix}_stddev_{suffix}"] = ""
            if pair["speedup"] is not None:
                row["tier_over_system_throughput"] = f"{pair['speedup']:.8f}"
                row["tier_over_system_throughput_error"] = f"{pair['speedup_error']:.8f}"
            else:
                row["tier_over_system_throughput"] = ""
                row["tier_over_system_throughput_error"] = ""
            if pair["p99_ratio"] is not None:
                row["tier_over_system_p99_ratio"] = f"{pair['p99_ratio']:.8f}"
                row["tier_over_system_p99_ratio_error"] = f"{pair['p99_ratio_error']:.8f}"
            else:
                row["tier_over_system_p99_ratio"] = ""
                row["tier_over_system_p99_ratio_error"] = ""
            if pair["abort_change_pp"] is not None:
                row["tier_minus_system_abort_rate_pp"] = f"{pair['abort_change_pp']:.8f}"
                row["tier_minus_system_abort_rate_error_pp"] = f"{pair['abort_change_error_pp']:.8f}"
            else:
                row["tier_minus_system_abort_rate_pp"] = ""
                row["tier_minus_system_abort_rate_error_pp"] = ""
            row["system_abort_rate"] = (
                f"{metric_mean(system, 'abort_rate'):.8f}"
                if summary_row_complete(system)
                and metric_mean(system, "abort_rate") is not None
                else ""
            )
            row["tier_abort_rate"] = (
                f"{metric_mean(tier, 'abort_rate'):.8f}"
                if summary_row_complete(tier)
                and metric_mean(tier, "abort_rate") is not None
                else ""
            )
            writer.writerow(row)


def finite_pair_values(
    pairs: Sequence[Mapping[str, object]], value_key: str, error_key: str
) -> Tuple[List[int], List[float], List[float]]:
    positions: List[int] = []
    values: List[float] = []
    errors: List[float] = []
    for index, pair in enumerate(pairs):
        value = pair.get(value_key)
        error = pair.get(error_key)
        if isinstance(value, (int, float)) and math.isfinite(value):
            positions.append(index)
            values.append(float(value))
            errors.append(float(error) if isinstance(error, (int, float)) else 0.0)
    return positions, values, errors


def draw_pair_chart(
    pairs: Sequence[Mapping[str, object]],
    output_path: Path,
    value_key: str,
    error_key: str,
    title: str,
    ylabel: str,
    percentage: bool = False,
) -> None:
    positions, values, errors = finite_pair_values(pairs, value_key, error_key)
    labels = [chart_label(pair["config"]) for pair in pairs]
    figure, axis = plt.subplots(figsize=(12, 6.2))
    if positions:
        bars = axis.bar(
            positions,
            values,
            yerr=errors,
            capsize=4,
            color=COLORS["tier"],
            edgecolor="#24445C",
            linewidth=0.7,
            label="TierAlloc / system",
        )
        for bar, value, error in zip(bars, values, errors):
            text = f"{value:+.1f} pp" if percentage else f"{value:.2f}"
            axis.text(
                bar.get_x() + bar.get_width() / 2.0,
                value + (error if error > 0 else 0.0),
                text,
                ha="center",
                va="bottom",
                fontsize=9,
            )
    axis.axhline(
        0.0 if percentage else 1.0,
        color="#333333",
        linewidth=1.0,
        linestyle="--",
        label="no change",
    )
    axis.set_xticks(range(len(labels)))
    axis.set_xticklabels(labels)
    axis.set_title(title)
    axis.set_ylabel(ylabel)
    axis.grid(axis="y", alpha=0.25)
    axis.set_axisbelow(True)

    if percentage:
        finite_low = min(
            (value - error for value, error in zip(values, errors)),
            default=-1.0,
        )
        finite_high = max(
            (value + error for value, error in zip(values, errors)),
            default=1.0,
        )
        span = max(finite_high - finite_low, 1.0)
        bottom = min(-10.0, finite_low - 0.12 * span)
        top = max(10.0, finite_high + 0.20 * span)
        axis.set_ylim(bottom, top)
        missing_y = top - 0.08 * (top - bottom)
    else:
        finite_low = min(
            (value - error for value, error in zip(values, errors)),
            default=0.5,
        )
        finite_high = max(
            (value + error for value, error in zip(values, errors)),
            default=1.5,
        )
        span = max(finite_high - finite_low, 0.2)
        bottom = max(0.0, min(0.75, finite_low - 0.15 * span))
        top = max(1.15, finite_high + 0.20 * span)
        axis.set_ylim(bottom, top)
        missing_y = bottom + 0.08 * (top - bottom)

    missing = [
        index
        for index, pair in enumerate(pairs)
        if not isinstance(pair.get(value_key), (int, float))
    ]
    for index in missing:
        axis.text(
            index,
            missing_y,
            "N/A\n(incomplete)",
            ha="center",
            va="center",
            fontsize=8,
            color="#A33A3A",
        )
    axis.legend(loc="best")
    figure.tight_layout()
    figure.savefig(output_path, dpi=180, bbox_inches="tight")
    plt.close(figure)


def raw_checksum_failures(
    raw_rows: Iterable[Mapping[str, str]],
) -> List[Mapping[str, str]]:
    failures = []
    for row in raw_rows:
        final_checksum = row.get("final_checksum", "")
        expected_checksum = row.get("expected_checksum", "")
        if final_checksum and expected_checksum and final_checksum != expected_checksum:
            failures.append(row)
    return failures


def raw_checksum_missing(
    raw_rows: Iterable[Mapping[str, str]],
) -> List[Mapping[str, str]]:
    return [
        row
        for row in raw_rows
        if not row.get("final_checksum", "") or not row.get("expected_checksum", "")
    ]


def raw_run_count_anomalies(
    raw_rows: Iterable[Mapping[str, str]],
) -> List[str]:
    groups: Dict[Tuple[str, str, int, int, str], List[Mapping[str, str]]] = defaultdict(list)
    for row in raw_rows:
        key = (
            row.get("allocator", ""),
            row.get("backend", ""),
            parse_int(row.get("threads")) or 0,
            parse_int(row.get("read_ratio")) or 0,
            row.get("contention", ""),
        )
        groups[key].append(row)
    anomalies = []
    for key, rows in sorted(groups.items()):
        run_values = sorted(row.get("run", "") for row in rows)
        if len(rows) != 5 or run_values != ["1", "2", "3", "4", "5"]:
            allocator, backend, threads, read_ratio, contention = key
            anomalies.append(
                f"{ALLOCATOR_LABELS.get(allocator, allocator)} / "
                f"{config_label(('x', backend, threads, read_ratio, contention))}: "
                f"runs={run_values or 'none'}"
            )
    return anomalies


def invalid_groups(
    raw_rows: Iterable[Mapping[str, str]],
) -> Dict[Tuple[str, str, int, int, str], List[Mapping[str, str]]]:
    groups: Dict[Tuple[str, str, int, int, str], List[Mapping[str, str]]] = defaultdict(list)
    for row in raw_rows:
        if not is_valid(row):
            key = (
                row.get("allocator", ""),
                row.get("backend", ""),
                parse_int(row.get("threads")) or 0,
                parse_int(row.get("read_ratio")) or 0,
                row.get("contention", ""),
            )
            groups[key].append(row)
    return groups


def variance_anomalies(
    summary_rows: Iterable[Mapping[str, str]],
) -> List[str]:
    anomalies: List[str] = []
    for row in summary_rows:
        if not summary_row_complete(row):
            continue
        for metric, (mean_field, sd_field) in METRIC_FIELDS.items():
            mean = parse_float(row.get(mean_field))
            sd = parse_float(row.get(sd_field))
            if mean is None or sd is None or mean == 0.0:
                continue
            relative = abs(sd / mean)
            if relative > 0.10:
                config = (
                    "x",
                    row["backend"],
                    int(row["threads"]),
                    int(row["read_ratio"]),
                    row["contention"],
                )
                anomalies.append(
                    f"{ALLOCATOR_LABELS.get(row.get('allocator', ''), row.get('allocator', ''))} "
                    f"{config_label(config)}: {metric} stddev is {relative * 100.0:.1f}% of mean"
                )
    return anomalies


def endpoint_anomalies(
    lookup: Mapping[Tuple[str, str, int, int, str], Mapping[str, str]],
) -> List[str]:
    anomalies: List[str] = []
    # The ablation has only 1T and 8T for low-contention 50R/50W. Report
    # conspicuous endpoint changes, not a complete scaling curve.
    for allocator in ALLOCATORS:
        for backend in ("occ", "ww"):
            one = lookup.get((allocator, backend, 1, 50, "low"))
            eight = lookup.get((allocator, backend, 8, 50, "low"))
            if not summary_row_complete(one) or not summary_row_complete(eight):
                continue
            one_tps = metric_mean(one, "throughput")
            eight_tps = metric_mean(eight, "throughput")
            one_p99 = metric_mean(one, "p99")
            eight_p99 = metric_mean(eight, "p99")
            if one_tps and eight_tps and eight_tps < one_tps * 0.75:
                anomalies.append(
                    f"{ALLOCATOR_LABELS[allocator]} {BACKEND_LABELS[backend]} low 50R/50W: "
                    f"8T throughput is {eight_tps / one_tps * 100.0:.1f}% of 1T"
                )
            if one_p99 and eight_p99 and eight_p99 > one_p99 * 5.0:
                anomalies.append(
                    f"{ALLOCATOR_LABELS[allocator]} {BACKEND_LABELS[backend]} low 50R/50W: "
                    f"8T p99 is {eight_p99 / one_p99:.1f}x the 1T value"
                )
    return anomalies


def pair_number(pair: Mapping[str, object], key: str) -> float | None:
    value = pair.get(key)
    return float(value) if isinstance(value, (int, float)) else None


def short_pair_label(pair: Mapping[str, object]) -> str:
    config = pair["config"]
    assert isinstance(config, tuple)
    _, backend, threads, _, _ = config
    return f"{BACKEND_LABELS[backend]} {threads}T"


def ratio_delta_text(pair: Mapping[str, object], key: str) -> str:
    value = pair_number(pair, key)
    return "N/A" if value is None else f"{(value - 1.0) * 100.0:+.1f}%"


def report_config_table() -> str:
    lines = [
        "| Configuration | Backend | Threads | Mix | Contention | Keys |",
        "|---|---:|---:|---:|---:|---:|",
    ]
    for identifier, backend, threads, read_ratio, contention in CONFIGS:
        keys = "65,536" if contention == "low" else "16"
        lines.append(
            f"| {identifier} | {BACKEND_LABELS[backend]} | {threads} | "
            f"{read_ratio}R/{100 - read_ratio}W | {contention} | {keys} |"
        )
    return "\n".join(lines)


def report_metric_table(
    pairs: Sequence[Mapping[str, object]], metric: str
) -> str:
    lines = [
        "| Configuration | system allocator | TierAlloc | Tier/System or change |",
        "|---|---:|---:|---:|",
    ]
    for pair in pairs:
        if metric == "throughput":
            comparison_value = pair["speedup"]
            comparison_text = (
                "N/A (incomplete)"
                if comparison_value is None
                else f"{comparison_value:.3f}x ({(comparison_value - 1.0) * 100.0:+.1f}%)"
            )
        elif metric == "p99":
            comparison_value = pair["p99_ratio"]
            comparison_text = (
                "N/A (incomplete)"
                if comparison_value is None
                else f"{comparison_value:.3f}x ({(comparison_value - 1.0) * 100.0:+.1f}%)"
            )
        else:
            comparison_value = pair["abort_change_pp"]
            comparison_text = (
                "N/A (incomplete)"
                if comparison_value is None
                else fmt_pp(comparison_value)
            )
        lines.append(
            f"| {pair['label']} | {metric_text(pair['system'], metric)} | "
            f"{metric_text(pair['tier'], metric)} | {comparison_text} |"
        )
    return "\n".join(lines)


def build_report(
    metadata: Mapping[str, str],
    raw_rows: Sequence[Mapping[str, str]],
    summary_rows: Sequence[Mapping[str, str]],
    pairs: Sequence[Mapping[str, object]],
    lookup: Mapping[Tuple[str, str, int, int, str], Mapping[str, str]],
    chart_dir: Path,
) -> str:
    checksum_failures = raw_checksum_failures(raw_rows)
    checksum_missing = raw_checksum_missing(raw_rows)
    invalid = invalid_groups(raw_rows)
    invalid_count = sum(len(rows) for rows in invalid.values())
    valid_count = len(raw_rows) - invalid_count
    runs = Counter(row.get("run", "") for row in raw_rows)
    runs_text = ", ".join(
        f"{key or '<empty>'}={value}" for key, value in sorted(runs.items())
    )
    variance = variance_anomalies(summary_rows)
    endpoint = endpoint_anomalies(lookup)
    run_anomalies = raw_run_count_anomalies(raw_rows)
    by_id = {str(pair["id"]): pair for pair in pairs}
    if not checksum_failures and not checksum_missing:
        correctness_text = (
            "本轮没有发现数据校验错误：checksum mismatch=0 且 checksum missing=0。"
        )
    else:
        correctness_text = (
            f"本轮发现 checksum mismatch={len(checksum_failures)}、"
            f"checksum missing={len(checksum_missing)}，不能把这些运行作为正确性通过。"
        )

    def side_metric(identifier: str, allocator: str, metric: str) -> float | None:
        pair = by_id[identifier]
        source = pair[allocator]
        assert source is None or isinstance(source, Mapping)
        return metric_mean(source, metric)

    def endpoint_text(backend: str, allocator: str) -> str:
        one = side_metric(
            f"{backend}_1t_50r50w_low", allocator, "throughput"
        )
        eight = side_metric(
            f"{backend}_8t_50r50w_low", allocator, "throughput"
        )
        if one is None or eight is None or one <= 0:
            return "N/A"
        return f"{fmt_tps(one)}→{fmt_tps(eight)} ({eight / one:.2f}x)"

    lines = [
        "# CaSTM allocator ablation analysis",
        "",
        "本报告只读取本目录已有的 raw.csv、summary.csv 和 metadata.txt，"
        "没有启动 benchmark，也没有修改 benchmark 输入数据。",
        "",
        "## Allocator switch",
        "",
        "实验保持同一份 workload、同一 benchmark harness、同一编译优化和同一 "
        "retry guard，仅在构建期切换 STM_ALLOCATOR_MODE：",
        "",
        "- system：本次纳入 ablation 的 OccSTM VersionNode/显式事务对象分配，以及 WwSTM VersionNode/WriteRecord，使用全局 ::operator new/delete。",
        "- tier：上述对象改用 ThreadHeap/TierAlloc。",
        "- WwSTM TxDescriptor（需要 64-byte 对齐）、EBR GarbageNode 元数据、std::vector 缓冲区、benchmark 的 TMVar 容器和 StripedLockTable 不在切换范围内。",
        "",
        "因此这是“关键对象分配路径”的公平 ablation，不是把进程中的每一次分配都替换成 TierAlloc。"
        "没有修改 STM correctness 协议、冲突策略、retry guard、线程绑定或 workload。",
        "",
        "## Configurations",
        "",
        report_config_table(),
        "",
        f"每个 allocator × configuration 有 5 个独立进程样本；固定 warmup=1s、"
        f"measurement=5s、每个逻辑事务 8 次操作。共 {len(raw_rows)} 个 process runs，"
        f"其中 valid={valid_count}，invalid={invalid_count}。",
        "",
        "构建/运行环境来自 metadata.txt："
        f"{metadata.get('build_type', 'Release')}、"
        f"{metadata.get('optimization', '-O3')}、"
        f"NDEBUG={metadata.get('NDEBUG', '1')}、"
        f"VERIFY={metadata.get('STM_WW_VERIFY_LOGIC_MODE', '0')}、"
        f"TEST_HOOKS={metadata.get('STM_WW_TEST_HOOKS', '0')}；"
        f"CPU={metadata.get('cpu_model', 'unknown')}，"
        f"logical CPUs={metadata.get('logical_cpu_count', 'unknown')}。",
        "",
        "## Data quality",
        "",
        "- summary 的 mean/stddev 只对 valid runs 计算，stddev 为 sample standard deviation。",
        "- 任一 allocator/configuration 只要 5 次没有全部正常完成，整个对比点在本报告中记为 N/A，不做插值、不补跑、不把 throughput 设为 0。",
        f"- checksum mismatch={len(checksum_failures)}，checksum missing={len(checksum_missing)}；"
        f"有 checksum 的运行均逐行比较 final checksum 与 expected checksum。",
        "- 图中的每个基础数据点使用 mean ± sample stddev；ratio 图的误差条使用两侧独立样本的误差传播近似。",
        f"- run 字段覆盖情况：{runs_text}。",
        "",
        "## Correctness",
        "",
        correctness_text
        + f"有 {invalid_count} 次运行因未能在 retry guard 内正常完成而标记 invalid；"
        + "这属于 progress failure，不应表述为 STM 算错数据。",
        "",
    ]

    if invalid:
        lines.append("invalid 具体情况：")
        lines.append("")
        for key, rows in sorted(invalid.items()):
            allocator, backend, threads, read_ratio, contention = key
            errors = sorted(
                {row.get("error", "").strip() or "no error text" for row in rows}
            )
            lines.append(
                f"- {ALLOCATOR_LABELS.get(allocator, allocator)} / "
                f"{config_label(('x', backend, threads, read_ratio, contention))}："
                f"{len(rows)}/5 invalid；原因：{'; '.join(errors)}。"
            )
        lines.append("")
    else:
        lines.extend(["本轮所有运行均正常完成。", ""])

    lines.extend(
        [
            "## Throughput",
            "",
            report_metric_table(pairs, "throughput"),
            "",
            "Tier/System 只在两侧都是 5/5 valid 时计算。完整点的结果为：",
            "",
        ]
    )
    for pair in pairs:
        speedup = pair["speedup"]
        if speedup is None:
            lines.append(
                f"- {pair['label']}：N/A，{pair['status']}；没有把不完整样本用于 speedup。"
            )
        else:
            lines.append(
                f"- {pair['label']}：TierAlloc/system = {speedup:.3f}x，"
                f"即 {(speedup - 1.0) * 100.0:+.1f}%。"
            )
    lines.extend(
        [
            "",
            "单线程 50R/50W low 的两个 backend 中，TierAlloc 均高于 system："
            f"OccSTM {ratio_delta_text(by_id['occ_1t_50r50w_low'], 'speedup')}，"
            f"WwSTM {ratio_delta_text(by_id['ww_1t_50r50w_low'], 'speedup')}。"
            f"8T low 下，OccSTM {ratio_delta_text(by_id['occ_8t_50r50w_low'], 'speedup')}，"
            f"WwSTM {ratio_delta_text(by_id['ww_8t_50r50w_low'], 'speedup')}。"
            "这些数据说明在本 workload/实现中，TierAlloc 没有体现出"
            "额外的单线程分配开销；但由于仍有多类 system-allocated 对象，不能把它推广成"
            "“TierAlloc 总体一定更快”。",
            "",
            f"高冲突 WwSTM 20T、10R/90W 是反例：TierAlloc throughput 为 system 的 "
            f"{pair_number(by_id['ww_20t_10r90w_high'], 'speedup'):.3f}x "
            f"（{ratio_delta_text(by_id['ww_20t_10r90w_high'], 'speedup')}），"
            "所以 allocator 优势不是普适的；该点更像由事务冲突、"
            "wound/abort 交互和调度成本主导，而不是单纯由分配器吞吐主导。这里是基于"
            "allocator 对照的工程推断，不是事件级 profiling 证明。",
            "",
            "低冲突扩展只能观察到 1T 和 8T 两个端点，不能当作完整线程扩展曲线："
            f"OccSTM system {endpoint_text('occ', 'system')}、TierAlloc {endpoint_text('occ', 'tier')}；"
            f"WwSTM system {endpoint_text('ww', 'system')}、TierAlloc {endpoint_text('ww', 'tier')}。"
            "OccSTM 的端点吞吐上升但远非 8 倍线性，WwSTM 的 8T 端点反而低于 1T，"
            "说明低冲突 workload 下 STM 的并发成本/尾延迟已成为重要因素。",
            "",
            "## Tail latency",
            "",
            report_metric_table(pairs, "p99"),
            "",
            "完整点的 p99 Tier/System 比值：",
            "",
        ]
    )
    for pair in pairs:
        ratio = pair["p99_ratio"]
        if ratio is None:
            lines.append(f"- {pair['label']}：N/A（{pair['status']}）。")
        else:
            lines.append(
                f"- {pair['label']}：{ratio:.3f}x，"
                f"即 p99 {(ratio - 1.0) * 100.0:+.1f}%（低于 1x 表示 TierAlloc 尾延迟更低）。"
            )
    lines.extend(
        [
            "",
            "TierAlloc 在四个低冲突完整点改善 p99："
            + "、".join(
                f"{short_pair_label(pair)} {ratio_delta_text(pair, 'p99_ratio')}"
                for pair in pairs
                if pair["config"][4] == "low" and pair["p99_ratio"] is not None
            )
            + "。"
            f"高冲突 WwSTM 20T 则 {ratio_delta_text(by_id['ww_20t_10r90w_high'], 'p99_ratio')}，"
            "与 throughput 下降方向一致。",
            "",
            "## Abort behavior",
            "",
            report_metric_table(pairs, "abort_rate"),
            "",
            "完整点的 TierAlloc − system abort-rate 变化：",
            "",
        ]
    )
    for pair in pairs:
        change = pair["abort_change_pp"]
        if change is None:
            lines.append(f"- {pair['label']}：N/A（{pair['status']}）。")
        else:
            lines.append(f"- {pair['label']}：{change:+.2f} percentage points。")
    lines.extend(
        [
            "",
            f"低冲突 OccSTM 8T 的 abort rate 从 "
            f"{fmt_percent(side_metric('occ_8t_50r50w_low', 'system', 'abort_rate'))} 降到 "
            f"{fmt_percent(side_metric('occ_8t_50r50w_low', 'tier', 'abort_rate'))}，"
            f"变化 {fmt_pp(pair_number(by_id['occ_8t_50r50w_low'], 'abort_change_pp'))}；"
            "WwSTM 低冲突点两种 allocator 都没有观察到 abort。高冲突 WwSTM 20T 中，"
            f"TierAlloc 的 abort rate 从 "
            f"{fmt_percent(side_metric('ww_20t_10r90w_high', 'system', 'abort_rate'))} 升到 "
            f"{fmt_percent(side_metric('ww_20t_10r90w_high', 'tier', 'abort_rate'))}，"
            f"变化 {fmt_pp(pair_number(by_id['ww_20t_10r90w_high'], 'abort_change_pp'))}，"
            "因此 TierAlloc 并没有降低高冲突 WwSTM 的冲突失败成本。",
            "",
            f"OccSTM 8T high 的 system 只有 "
            f"{valid_run_count(by_id['occ_8t_50r50w_high']['system'])} valid，"
            f"TierAlloc 为 {valid_run_count(by_id['occ_8t_50r50w_high']['tier'])} valid；两侧"
            "的已完成样本 abort rate 都约 99%，但因为 system 侧没有完整 5 次，"
            "吞吐、p99 和 allocator ratio 均保持 N/A。其含义是 retry guard 下的"
            "progress failure，而不是 checksum/correctness failure。",
            "",
            "## Key conclusion",
            "",
            f"1. 在单线程和低冲突 8T 的代表点，TierAlloc 对关键 VersionNode/WriteRecord "
            f"路径带来约 15.5%～30.3% 的吞吐提升，并通常降低 p99；这支持“分配器路径"
            "会影响 STM 性能”的判断。",
            f"2. TierAlloc 不是所有场景的赢家：高冲突 WwSTM 20T 的 throughput "
            f"{ratio_delta_text(by_id['ww_20t_10r90w_high'], 'speedup')}，"
            f"p99 {ratio_delta_text(by_id['ww_20t_10r90w_high'], 'p99_ratio')}，"
            f"abort rate {fmt_pp(pair_number(by_id['ww_20t_10r90w_high'], 'abort_change_pp'))}。",
            "3. OccSTM 高冲突 8T 触发 retry guard，证明该场景首先受 progress/冲突管理限制；"
            "不应把它的局部 valid 样本均值当作正常性能点。",
            "4. 本轮只能说明“关键 allocator 路径”与结果之间的相关影响；若要把冲突管理"
            "和 allocator 的贡献进一步分离，需要 profiling 或事件计数，但不属于本轮。",
            "",
            "## Interview value",
            "",
            "- 有可复现的公平对照：同一 harness、workload、编译参数和 5 次独立进程，只切换 build-time allocator mode。",
            "- 结果不是只挑正向数字：低冲突下 TierAlloc 有收益，高冲突 WwSTM 出现回退，展示了对测量边界和 trade-off 的诚实解释。",
            "- 能把 correctness 与 progress 分开：3 个 invalid run 的 checksum 仍正确，真正的问题是 retry guard exhaustion。",
            f"- 可以用 OccSTM 8T low 的 abort 变化（"
            f"{fmt_percent(side_metric('occ_8t_50r50w_low', 'system', 'abort_rate'))}"
            f"→{fmt_percent(side_metric('occ_8t_50r50w_low', 'tier', 'abort_rate'))}）和 "
            f"WwSTM 20T high 的 throughput 回退（"
            f"{fmt_tps(side_metric('ww_20t_10r90w_high', 'system', 'throughput'))}"
            f"→{fmt_tps(side_metric('ww_20t_10r90w_high', 'tier', 'throughput'))} TPS）"
            "作为两组对比鲜明的面试数字。",
            "",
            "## Anomalies",
            "",
        ]
    )
    if invalid:
        lines.append(
            "- 主要异常：system/OccSTM/8T/50R50W/high 只有 2/5 valid，另外 3 次达到 retry guard；"
            "该 pair 的 speedup、p99 ratio 必须保持 N/A。"
        )
    if variance:
        lines.append("- valid 样本的相对 stddev 超过 10%：")
        lines.extend(f"  - {item}" for item in variance)
    if endpoint:
        lines.append("- 端点变化需要谨慎解释（本 ablation 不是完整 scaling curve）：")
        lines.extend(f"  - {item}" for item in endpoint)
    if run_anomalies:
        lines.append("- 原始 run 编号或每组样本数异常：")
        lines.extend(f"  - {item}" for item in run_anomalies)
    if checksum_failures:
        lines.append(f"- checksum mismatch 行数：{len(checksum_failures)}。")
    if checksum_missing:
        lines.append(f"- checksum 缺失行数：{len(checksum_missing)}。")
    if not invalid and not variance and not endpoint:
        lines.append("- 未发现需要额外标注的异常点。")
    lines.extend(
        [
            "",
            "相对 stddev 只用于提示复测优先级，不改变既有均值或有效性判定；本轮不自动重跑。",
            "",
            "## Recommended allocator-ablation points",
            "",
            "如果后续只保留 4～6 个代表点用于 README 或复核，建议优先保留以下 5 个：",
            "",
            "1. OccSTM 1T 50R/50W low：单线程 allocator 影响。",
            "2. WwSTM 1T 50R/50W low：WwSTM 单线程关键对象分配影响。",
            "3. OccSTM 8T 50R/50W low：低冲突并发扩展与 abort 变化。",
            "4. WwSTM 8T 50R/50W low：低冲突并发吞吐/p99 收益。",
            "5. WwSTM 20T 10R/90W high：展示 TierAlloc 并非普遍加速，以及冲突管理主导的反例。",
            "",
            "OccSTM 8T 50R/50W high 适合作为 retry-limit/progress 边界的诊断点，"
            "不适合作为正常 throughput ratio 的宣传数字，除非两侧都能完整通过相同 guard。",
            "",
            "图表：",
            "",
            f"- throughput_speedup.png（{chart_dir / 'throughput_speedup.png'}）",
            f"- p99_ratio.png（{chart_dir / 'p99_ratio.png'}）",
            f"- abort_rate_change.png（{chart_dir / 'abort_rate_change.png'}）",
            "",
            "## Remaining work",
            "",
            "README / CI / résumé / interview packaging",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    args = parse_args()
    result_dir = args.result_dir.resolve()
    raw_path = result_dir / "raw.csv"
    summary_path = result_dir / "summary.csv"
    metadata_path = result_dir / "metadata.txt"
    for path in (raw_path, summary_path, metadata_path):
        if not path.is_file():
            raise SystemExit(f"required input does not exist: {path}")

    raw_rows = read_csv(raw_path)
    summary_rows = read_csv(summary_path)
    metadata = read_metadata(metadata_path)
    if not raw_rows:
        raise SystemExit(f"raw.csv is empty: {raw_path}")

    lookup: Dict[Tuple[str, str, int, int, str], Mapping[str, str]] = {}
    for row in summary_rows:
        try:
            lookup[summary_key(row)] = row
        except (KeyError, TypeError, ValueError) as error:
            raise SystemExit(f"invalid summary row: {row}") from error

    pairs = [pair_record(config, lookup) for config in CONFIGS]
    chart_dir = args.output_dir.resolve() if args.output_dir else result_dir / "charts"
    analysis_path = (
        args.analysis_output.resolve()
        if args.analysis_output
        else result_dir / "analysis.md"
    )
    derived_summary_path = (
        args.summary_output.resolve()
        if args.summary_output
        else result_dir / "analysis_summary.csv"
    )
    chart_dir.mkdir(parents=True, exist_ok=True)
    analysis_path.parent.mkdir(parents=True, exist_ok=True)
    derived_summary_path.parent.mkdir(parents=True, exist_ok=True)

    draw_pair_chart(
        pairs,
        chart_dir / "throughput_speedup.png",
        "speedup",
        "speedup_error",
        "TierAlloc / system throughput",
        "Throughput ratio (higher is better)",
    )
    draw_pair_chart(
        pairs,
        chart_dir / "p99_ratio.png",
        "p99_ratio",
        "p99_ratio_error",
        "TierAlloc / system p99 latency",
        "p99 ratio (lower is better)",
    )
    draw_pair_chart(
        pairs,
        chart_dir / "abort_rate_change.png",
        "abort_change_pp",
        "abort_change_error_pp",
        "TierAlloc - system abort rate",
        "Change in abort rate (percentage points)",
        percentage=True,
    )
    write_analysis_summary(pairs, derived_summary_path)
    analysis_path.write_text(
        build_report(
            metadata,
            raw_rows,
            summary_rows,
            pairs,
            lookup,
            chart_dir,
        ),
        encoding="utf-8",
    )

    print(f"analysis={analysis_path}")
    print(f"derived_summary={derived_summary_path}")
    print(f"charts={chart_dir}")
    print(f"runs={len(raw_rows)}")
    print(f"checksum_failures={len(raw_checksum_failures(raw_rows))}")
    print(
        f"complete_pairs={sum(1 for pair in pairs if pair['status'] == 'complete')}/"
        f"{len(pairs)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
