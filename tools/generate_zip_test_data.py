#!/usr/bin/env python3
# -*- coding: utf-8 -*-
from __future__ import annotations

import argparse
import json
import math
import os
import random
import shutil
import statistics
import time
import zipfile
from pathlib import Path

EN_DIRS = [
    "docs", "images", "build", "cache", "config", "source", "assets", "logs",
    "release", "debug", "reports", "temp", "scripts", "vendor", "public",
]
ZH_DIRS = [
    "文档", "图片", "配置", "缓存", "源码", "资源", "日志", "发布",
    "调试", "报表", "脚本", "下载", "上传", "归档", "测试",
]
EN_STEMS = [
    "report", "summary", "sample", "record", "entry", "image", "build", "asset",
    "index", "detail", "config", "output", "result", "trace", "module",
]
ZH_STEMS = [
    "说明", "记录", "样本", "条目", "截图", "索引", "配置", "输出",
    "结果", "细节", "模块", "归档", "清单", "资源", "测试",
]
TEXT_EXTS = ["txt", "md", "json", "csv", "log", "xml", "ini"]
BIN_EXTS = ["bin", "dat", "cache", "blob", "pak"]
TEXT_WORDS = [
    "alpha", "delta", "index", "sample", "archive", "entry", "random",
    "payload", "trace", "module", "render", "window", "token", "metric",
]
ZH_WORDS = ["测试", "数据", "中文", "样本", "条目", "目录", "结果", "内容", "压缩", "归档"]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate EveryZip zip test dataset.")
    parser.add_argument("--output-root", required=True, help="Output directory, for example G:\\EveryZipTestData")
    parser.add_argument("--zip-count", type=int, required=True, help="Number of zip files to create")
    parser.add_argument("--entry-count", type=int, required=True, help="Total entries across all zip files")
    parser.add_argument("--seed", type=int, default=20260531, help="Random seed")
    parser.add_argument("--batch-size", type=int, default=250, help="Subdirectory fanout per batch")
    parser.add_argument("--sample-ratio-zh", type=float, default=0.30, help="Approximate Chinese name ratio")
    parser.add_argument("--min-text", type=int, default=6, help="Minimum text payload bytes")
    parser.add_argument("--max-text", type=int, default=28, help="Maximum text payload bytes")
    parser.add_argument("--min-bin", type=int, default=10, help="Minimum binary payload bytes")
    parser.add_argument("--max-bin", type=int, default=28, help="Maximum binary payload bytes")
    parser.add_argument("--binary-ratio", type=float, default=0.05, help="Approximate binary payload ratio")
    parser.add_argument("--progress-every", type=int, default=500, help="Print progress every N zip files")
    parser.add_argument("--report-json", default="", help="Optional JSON report output path")
    parser.add_argument("--clean", action="store_true", help="Delete output directory before generation")
    parser.add_argument("--verify-only", action="store_true", help="Only verify an existing dataset")
    parser.add_argument("--compact-names", action="store_true", help="Use short names to reduce ZIP metadata overhead")
    return parser.parse_args()


def allocate_entries(zip_count: int, total_entries: int, rng: random.Random) -> list[int]:
    base = total_entries // zip_count
    counts: list[int] = []
    for _ in range(zip_count):
        delta = rng.randint(-28, 28)
        counts.append(max(1, base + delta))
    current = sum(counts)
    diff = total_entries - current
    index = 0
    step = 1 if diff > 0 else -1
    while diff != 0:
        slot = index % zip_count
        next_value = counts[slot] + step
        if next_value >= 1:
            counts[slot] = next_value
            diff -= step
        index += 1
    return counts


def choose_name(rng: random.Random, zh_ratio: float, kind: str) -> tuple[str, bool]:
    use_zh = rng.random() < zh_ratio
    if kind == "dir":
        pool = ZH_DIRS if use_zh else EN_DIRS
    else:
        pool = ZH_STEMS if use_zh else EN_STEMS
    return rng.choice(pool), use_zh


def build_entry_path(rng: random.Random, zh_ratio: float, index: int, compact: bool) -> tuple[str, int, int]:
    if compact:
        return build_compact_entry_path(rng, zh_ratio, index)

    depth = rng.randint(1, 3)
    parts: list[str] = []
    zh_hits = 0
    total_parts = 0
    for _ in range(depth):
        name, is_zh = choose_name(rng, zh_ratio, "dir")
        parts.append(f"{name}_{rng.randint(0, 9999):04d}")
        zh_hits += 1 if is_zh else 0
        total_parts += 1
    stem, is_zh = choose_name(rng, zh_ratio, "file")
    zh_hits += 1 if is_zh else 0
    total_parts += 1
    ext = rng.choice(TEXT_EXTS) if rng.random() < 0.78 else rng.choice(BIN_EXTS)
    filename = f"{stem}_{index:07d}_{rng.randint(0, 999999):06d}.{ext}"
    parts.append(filename)
    return "/".join(parts), zh_hits, total_parts


def build_compact_entry_path(rng: random.Random, zh_ratio: float, index: int) -> tuple[str, int, int]:
    dir_is_zh = rng.random() < zh_ratio
    file_is_zh = rng.random() < zh_ratio
    dir_prefix = "中" if dir_is_zh else "d"
    file_prefix = "名" if file_is_zh else "f"
    ext = rng.choice(TEXT_EXTS if rng.random() < 0.9 else BIN_EXTS[:2])
    dirname = f"{dir_prefix}{rng.randint(0, 999):03d}"
    filename = f"{file_prefix}{index:07d}.{ext}"
    zh_hits = (1 if dir_is_zh else 0) + (1 if file_is_zh else 0)
    return f"{dirname}/{filename}", zh_hits, 2


def make_text_payload(rng: random.Random, min_size: int, max_size: int) -> bytes:
    target = rng.randint(min_size, max_size)
    chunks: list[str] = []
    while len(" ".join(chunks).encode("utf-8")) < target:
        token = rng.choice(TEXT_WORDS)
        if rng.random() < 0.18:
            token += rng.choice(ZH_WORDS)
        if rng.random() < 0.35:
            token += str(rng.randint(100, 99999))
        chunks.append(token)
    return " ".join(chunks).encode("utf-8")


def make_binary_payload(rng: random.Random, min_size: int, max_size: int) -> bytes:
    target = rng.randint(min_size, max_size)
    block_len = rng.randint(8, 24)
    block = bytearray()
    while len(block) < block_len:
        block.extend(rng.randbytes(min(4, block_len - len(block))))
    repeats = math.ceil(target / len(block))
    return (bytes(block) * repeats)[:target]


def ensure_clean_dir(path: Path) -> None:
    if path.exists():
        shutil.rmtree(path)
    path.mkdir(parents=True, exist_ok=True)


def dir_size_bytes(root: Path) -> int:
    total = 0
    for base, _, files in os.walk(root):
        for name in files:
            total += (Path(base) / name).stat().st_size
    return total


def generate_dataset(args: argparse.Namespace) -> dict:
    rng = random.Random(args.seed)
    output_root = Path(args.output_root)
    if args.clean:
        ensure_clean_dir(output_root)
    else:
        output_root.mkdir(parents=True, exist_ok=True)

    counts = allocate_entries(args.zip_count, args.entry_count, rng)
    start = time.time()
    total_entries = 0
    total_name_parts = 0
    total_zh_parts = 0
    zip_sizes: list[int] = []

    for zip_index, entry_target in enumerate(counts):
        batch_dir = output_root / f"batch_{(zip_index // args.batch_size) + 1:04d}"
        batch_dir.mkdir(parents=True, exist_ok=True)
        zip_path = batch_dir / f"archive_{zip_index + 1:05d}.zip"
        zip_rng = random.Random(args.seed ^ ((zip_index + 1) * 0x9E3779B1))
        with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
            for entry_offset in range(entry_target):
                global_index = total_entries + entry_offset
                entry_name, zh_hits, part_count = build_entry_path(
                    zip_rng,
                    args.sample_ratio_zh,
                    global_index,
                    args.compact_names,
                )
                total_zh_parts += zh_hits
                total_name_parts += part_count
                if zip_rng.random() < args.binary_ratio or entry_name.rsplit(".", 1)[-1] in BIN_EXTS:
                    payload = make_binary_payload(zip_rng, args.min_bin, args.max_bin)
                else:
                    payload = make_text_payload(zip_rng, args.min_text, args.max_text)
                info = zipfile.ZipInfo(entry_name)
                info.compress_type = zipfile.ZIP_DEFLATED
                info.date_time = (2026, 5, 31, zip_rng.randint(0, 23), zip_rng.randint(0, 59), zip_rng.randint(0, 59))
                info.flag_bits |= 0x800
                zf.writestr(info, payload)
        total_entries += entry_target
        zip_sizes.append(zip_path.stat().st_size)
        if (zip_index + 1) % args.progress_every == 0 or (zip_index + 1) == args.zip_count:
            elapsed = max(time.time() - start, 0.001)
            written_bytes = sum(zip_sizes)
            bytes_per_entry = written_bytes / max(total_entries, 1)
            predicted_total = bytes_per_entry * args.entry_count
            zh_ratio_now = total_zh_parts / max(total_name_parts, 1)
            print(
                f"[progress] zip={zip_index + 1}/{args.zip_count} "
                f"entries={total_entries}/{args.entry_count} "
                f"size_mb={written_bytes / (1024 * 1024):.2f} "
                f"predict_mb={predicted_total / (1024 * 1024):.2f} "
                f"zh_ratio={zh_ratio_now:.4f} "
                f"elapsed_s={elapsed:.1f}",
                flush=True,
            )

    final_size = dir_size_bytes(output_root)
    return {
        "output_root": str(output_root),
        "zip_count": args.zip_count,
        "entry_count": total_entries,
        "size_bytes": final_size,
        "size_mb": final_size / (1024 * 1024),
        "zh_ratio": total_zh_parts / max(total_name_parts, 1),
        "elapsed_seconds": time.time() - start,
        "avg_zip_size_bytes": statistics.mean(zip_sizes) if zip_sizes else 0,
        "median_zip_size_bytes": statistics.median(zip_sizes) if zip_sizes else 0,
        "min_zip_size_bytes": min(zip_sizes) if zip_sizes else 0,
        "max_zip_size_bytes": max(zip_sizes) if zip_sizes else 0,
        "seed": args.seed,
        "params": {
            "sample_ratio_zh": args.sample_ratio_zh,
            "min_text": args.min_text,
            "max_text": args.max_text,
            "min_bin": args.min_bin,
            "max_bin": args.max_bin,
            "binary_ratio": args.binary_ratio,
            "batch_size": args.batch_size,
            "compact_names": args.compact_names,
        },
    }


def verify_dataset(output_root: Path, expected_zips: int, expected_entries: int, sample_verify: int = 8) -> dict:
    zip_paths = sorted(output_root.rglob("*.zip"))
    actual_entries = 0
    zh_parts = 0
    total_parts = 0
    sample_results = []
    for i, zip_path in enumerate(zip_paths):
        with zipfile.ZipFile(zip_path, "r") as zf:
            names = zf.namelist()
            actual_entries += len(names)
            if i < sample_verify:
                sample_results.append({"zip": str(zip_path), "entries": len(names), "first": names[:3]})
            for name in names:
                parts = [p for p in name.split("/") if p]
                total_parts += len(parts)
                zh_parts += sum(1 for p in parts if any("\u4e00" <= ch <= "\u9fff" for ch in p))
    size_bytes = dir_size_bytes(output_root)
    return {
        "zip_count": len(zip_paths),
        "entry_count": actual_entries,
        "size_bytes": size_bytes,
        "size_mb": size_bytes / (1024 * 1024),
        "zh_ratio": zh_parts / max(total_parts, 1),
        "zip_count_ok": len(zip_paths) == expected_zips,
        "entry_count_ok": actual_entries == expected_entries,
        "sample_results": sample_results,
    }


def main() -> int:
    args = parse_args()
    output_root = Path(args.output_root)
    if args.verify_only:
        report = verify_dataset(output_root, args.zip_count, args.entry_count)
    else:
        report = generate_dataset(args)
        report["verify"] = verify_dataset(output_root, args.zip_count, args.entry_count)

    text = json.dumps(report, ensure_ascii=False, indent=2)
    print(text)
    if args.report_json:
        Path(args.report_json).write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
