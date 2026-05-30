#!/usr/bin/env python3
import argparse
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SAMPLE = ROOT / "test_data" / "files_query_syntax.txt"
DB = ROOT / "test_data" / "files_query_syntax_test.ezdb"


CASES = [
    ("发票", 0, 3),
    ("合同 甲方", 0, 2),
    ('"用户协议"', 0, 1),
    ("张*明", 0, 3),
    ("第?章", 0, 4),
    ("合同 !草稿", 0, 2),
    ("发票 | 收据", 0, 5),
    ("(发票 | 收据) 报销", 0, 5),
    ('"a*b"', 0, 1),
    ('"第?章"', 0, 1),
    ("!草稿", 5, 5),
    ("*", 3, 3),
    ("??", 3, 3),
    ('"用户协议', 0, 0),
    ("(发票 | 收据", 0, 0),
]


SAMPLE_ROWS = [
    r"D:\测试\报销\发票_合同_甲方_张小明_第一章_用户协议.txt,100,1700000001",
    r"D:\测试\报销\发票_合同_甲方_张大明_第二章.txt,101,1700000002",
    r"D:\测试\报销\发票_张三明_第三章_a*b.txt,102,1700000003",
    r"D:\测试\报销\收据_第?章.txt,103,1700000004",
    r"D:\测试\报销\收据_其他.txt,104,1700000005",
]


def ensure_sample(path):
    if path.exists():
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(SAMPLE_ROWS) + "\n", encoding="utf-8")


def run(args):
    completed = subprocess.run(
        args,
        cwd=ROOT,
        text=True,
        encoding="utf-8",
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    if completed.returncode != 0:
        raise RuntimeError(f"command failed ({completed.returncode}): {' '.join(map(str, args))}\n{completed.stdout}")
    return completed.stdout


def returned_count(output):
    for line in output.splitlines():
        if line.startswith("returned:"):
            return int(line.split(":", 1)[1].strip())
    raise RuntimeError(f"missing returned line:\n{output}")


def main():
    parser = argparse.ArgumentParser(description="Run ezdb query syntax regression tests.")
    parser.add_argument("--bench", default=str(ROOT / "cmake-build-codex" / "EzdbBench.exe"))
    parser.add_argument("--db", default=str(DB))
    args = parser.parse_args()

    bench = Path(args.bench)
    if not bench.exists():
        print(f"bench not found: {bench}", file=sys.stderr)
        return 2

    db = Path(args.db)
    ensure_sample(SAMPLE)
    run([str(bench), "build", str(SAMPLE), str(db)])

    failed = 0
    for query, limit, expected in CASES:
        output = run([str(bench), "search", str(db), query, str(limit)])
        actual = returned_count(output)
        status = "ok" if actual == expected else "FAIL"
        print(f"{status}: {query!r} limit={limit} returned={actual} expected={expected}")
        if actual != expected:
            failed += 1

    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
