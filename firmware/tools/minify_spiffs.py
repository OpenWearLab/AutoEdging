#!/usr/bin/env python3
"""Prepare minified SPIFFS assets for ESP-IDF build.

This script performs conservative minification:
- HTML: remove comments and collapse inter-tag whitespace.
- CSS: remove comments and compact token whitespace.
- JS: keep behavior-safe trimming (remove trailing spaces/blank lines).

Other files are copied as-is.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import shutil
import sys


def minify_html(content: str) -> str:
    # Remove regular HTML comments (keep conditional comments).
    content = re.sub(r"<!--(?!\[if).*?-->", "", content, flags=re.S)
    # Remove whitespace between tags.
    content = re.sub(r">\s+<", "><", content)
    # Keep text compact but readable for debugging.
    content = re.sub(r"[ \t]+\n", "\n", content)
    content = re.sub(r"\n{2,}", "\n", content)
    return content.strip() + "\n"


def minify_css(content: str) -> str:
    content = re.sub(r"/\*.*?\*/", "", content, flags=re.S)
    content = re.sub(r"\s+", " ", content)
    content = re.sub(r"\s*([{}:;,>])\s*", r"\1", content)
    content = re.sub(r";}", "}", content)
    return content.strip() + "\n"


def minify_js(content: str) -> str:
    # Conservative path: avoid aggressive JS token rewriting.
    lines = [line.rstrip() for line in content.splitlines()]
    compact_lines = [line for line in lines if line.strip()]
    return ("\n".join(compact_lines).strip() + "\n") if compact_lines else ""


def transform_content(path: pathlib.Path, content: str) -> str:
    ext = path.suffix.lower()
    if ext == ".html":
        return minify_html(content)
    if ext == ".css":
        return minify_css(content)
    if ext == ".js":
        return minify_js(content)
    return content


def process_tree(src_dir: pathlib.Path, dst_dir: pathlib.Path) -> tuple[int, int]:
    total_src = 0
    total_dst = 0

    if dst_dir.exists():
        shutil.rmtree(dst_dir)
    dst_dir.mkdir(parents=True, exist_ok=True)

    for src in sorted(src_dir.rglob("*")):
        rel = src.relative_to(src_dir)
        dst = dst_dir / rel
        if src.is_dir():
            dst.mkdir(parents=True, exist_ok=True)
            continue

        total_src += src.stat().st_size
        dst.parent.mkdir(parents=True, exist_ok=True)

        ext = src.suffix.lower()
        if ext in {".html", ".css", ".js"}:
            content = src.read_text(encoding="utf-8")
            output = transform_content(src, content)
            dst.write_text(output, encoding="utf-8", newline="\n")
        else:
            shutil.copy2(src, dst)

        total_dst += dst.stat().st_size

    return total_src, total_dst


def main() -> int:
    parser = argparse.ArgumentParser(description="Minify SPIFFS web assets")
    parser.add_argument("--src", required=True, help="Source SPIFFS directory")
    parser.add_argument("--dst", required=True, help="Output directory")
    args = parser.parse_args()

    src_dir = pathlib.Path(args.src).resolve()
    dst_dir = pathlib.Path(args.dst).resolve()

    if not src_dir.exists() or not src_dir.is_dir():
        print(f"[webui-minify] source directory not found: {src_dir}", file=sys.stderr)
        return 1

    src_size, dst_size = process_tree(src_dir, dst_dir)
    ratio = (100.0 * dst_size / src_size) if src_size else 100.0
    print(
        f"[webui-minify] {src_size} -> {dst_size} bytes "
        f"({ratio:.1f}%), output: {dst_dir}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
