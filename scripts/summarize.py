#!/usr/bin/env python3
"""把 --matrix 产出的 exp_<id>.json 汇总成 Markdown 对照报告。

跨实验的横向汇总不属系统功能（§12），由本脚本离线生成：按扫描策略
分组、替换策略为行，输出每组的指标对照表。用法：

    python3 scripts/summarize.py results/*.json

shared runner（或任何虚拟环境）的性能有波动，绝对数字以本地为准；
同一轮次内各策略的对照是公平的，横向相对结论有效。
"""

import glob
import json
import sys
from pathlib import Path

REPLACER_ORDER = ["lru", "clock", "mru", "arc", "lru_k"]
POLICY_ORDER = ["baseline", "hotfirst", "defer_chain", "defer_paged"]

# 表格列：JSON 键名 -> 表头（单位已编码进键名）。
COLUMNS = [
    ("scan_count", "scans"),
    ("scan_latency_ms_p50", "scan p50 (ms)"),
    ("scan_latency_ms_p99", "scan p99 (ms)"),
    ("main_scan_ms_p50", "main p50 (ms)"),
    ("resolve_ms_p50", "resolve p50 (ms)"),
    ("base_hit_rate", "base hit"),
    ("base_logical_reads", "base reads"),
    ("base_physical_read_ops", "base read ops"),
    ("undo_hit_rate", "undo hit"),
    ("undo_logical_reads", "undo reads"),
    ("tp_throughput_txn_per_s", "tp txn/s"),
    ("write_conflict_rate", "conflict"),
    ("chain_avg_hops", "chain hops"),
    ("undo_file_mb_end", "undo MB"),
]


def Load(paths):
    summaries = []
    for path in paths:
        with open(path, encoding="utf-8") as file:
            summaries.append(json.load(file))
    if not summaries:
        raise SystemExit("no input JSON files")
    return summaries


def Format(value):
    if isinstance(value, float):
        return f"{value:.3f}"
    return str(value)


def GroupByPolicy(summaries):
    groups = {}
    for summary in summaries:
        policy = summary["scan_policy"]
        groups.setdefault(policy, []).append(summary)
    for policy in groups:
        groups[policy].sort(
            key=lambda s: REPLACER_ORDER.index(s["replacer"])
            if s["replacer"] in REPLACER_ORDER
            else len(REPLACER_ORDER)
        )
    ordered = [p for p in POLICY_ORDER if p in groups]
    ordered += sorted(p for p in groups if p not in POLICY_ORDER)
    return [(policy, groups[policy]) for policy in ordered]


def Table(rows):
    headers = ["replacer"] + [header for _, header in COLUMNS]
    lines = [
        "| " + " | ".join(headers) + " |",
        "|" + "|".join("---" for _ in headers) + "|",
    ]
    for summary in rows:
        cells = [summary["replacer"]]
        cells += [Format(summary.get(key, "-")) for key, _ in COLUMNS]
        lines.append("| " + " | ".join(cells) + " |")
    return "\n".join(lines)


def Report(summaries):
    first = summaries[0]
    lines = [
        "## Benchmark 矩阵报告",
        "",
        f"共 {len(summaries)} 轮（{first.get('page_size_kb', '?')} KB 页 × "
        f"{first.get('row_size_b', '?')} B 行 × {first.get('table_rows', '?')} 行，"
        f"tp_threads={first.get('tp_threads', '?')}，key_dist={first.get('key_dist', '?')}，"
        f"key_mapping={first.get('key_mapping', '?')}，duration_s={first.get('duration_s', '?')}）。",
        "",
        "命中率 = 1 − 逻辑未命中/逻辑读（§12）；reads 为扫描端逻辑读，read ops 为设备读调用次数，",
        "两者之比即预取摊销倍数。shared runner 性能有波动，绝对数字仅供参考，",
        "同一环境内策略间的横向对比是公平的。",
        "",
    ]
    for policy, rows in GroupByPolicy(summaries):
        lines += [f"### 扫描策略：{policy}", "", Table(rows), ""]
    return "\n".join(lines)


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    paths = []
    for arg in sys.argv[1:]:
        if any(c in arg for c in "*?["):
            paths += sorted(glob.glob(arg))
        else:
            paths.append(arg)
    print(Report(Load(paths)))


if __name__ == "__main__":
    main()
