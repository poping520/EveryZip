#!/usr/bin/env python3
"""List all files on this computer and save path, size, modified_time to txt."""

from __future__ import annotations

import argparse
import ctypes
import os
from pathlib import Path


def get_windows_drives() -> list[Path]:
    drives: list[Path] = []
    bitmask = ctypes.windll.kernel32.GetLogicalDrives()

    for index in range(26):
        if bitmask & (1 << index):
            drives.append(Path(f"{chr(65 + index)}:\\"))

    return drives


def iter_files(root: Path):
    try:
        with os.scandir(root) as entries:
            for entry in entries:
                try:
                    if entry.is_dir(follow_symlinks=False):
                        yield from iter_files(Path(entry.path))
                    elif entry.is_file(follow_symlinks=False):
                        yield Path(entry.path), entry.stat(follow_symlinks=False)
                except (OSError, PermissionError):
                    continue
    except (OSError, PermissionError):
        return


def non_negative_int(value: str) -> int:
    number = int(value)
    if number < 0:
        raise argparse.ArgumentTypeError("limit must be greater than or equal to 0")
    return number


def write_file_list(output_path: Path, roots: list[Path], limit: int | None) -> int:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    written_count = 0

    with output_path.open("w", encoding="utf-8", newline="\n") as output:
        for root in roots:
            if not root.exists():
                continue

            for path, stat in iter_files(root):
                if limit is not None and written_count >= limit:
                    return written_count

                output.write(f"{path}, {stat.st_size}, {int(stat.st_mtime)}\n")
                written_count += 1

    return written_count


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="List files and save one record per line: path, size, modified_time."
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=Path("all_files.txt"),
        help="Output txt path. Default: all_files.txt",
    )
    parser.add_argument(
        "-n",
        "--limit",
        type=non_negative_int,
        default=None,
        help="Maximum number of records to save. Default: no limit.",
    )
    parser.add_argument(
        "roots",
        nargs="*",
        type=Path,
        help="Root paths to scan. Default: all Windows drives.",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    roots = args.roots or get_windows_drives()
    written_count = write_file_list(args.output, roots, args.limit)
    print(f"Saved {written_count} records to: {args.output.resolve()}")


if __name__ == "__main__":
    main()
