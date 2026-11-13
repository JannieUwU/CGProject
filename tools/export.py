"""Dependency-free exports for display RGBA8 and benchmark records."""
import csv
import json
import math
import struct
import zlib
from pathlib import Path

def write_png(path, width, height, rgba):
    if width <= 0 or height <= 0 or len(rgba) != width * height * 4:
        raise ValueError("Expected positive dimensions and top-first RGBA8 bytes")
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
    stride = width * 4
    raw = b"".join(b"\0" + bytes(rgba[y*stride:(y+1)*stride]) for y in range(height))
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))

def write_results(directory, records, metadata):
    """Each record is one timed frame; metadata includes machine/build/thread settings and snapshot."""
    required = {"preset", "mode", "width", "height", "iteration", "kernel_ms", "shading_calls", "coverage_tests", "buffer_bytes"}
    rows = list(records)
    for row in rows:
        if set(row) != required or not math.isfinite(float(row["kernel_ms"])) or float(row["kernel_ms"]) < 0:
            raise ValueError("Invalid benchmark record")
    encoded = json.dumps(metadata, ensure_ascii=False, indent=2, allow_nan=False)
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    with (directory / "frames.csv").open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.DictWriter(stream, fieldnames=sorted(required))
        writer.writeheader()
        writer.writerows(rows)
    (directory / "parameters.json").write_text(encoded, encoding="utf-8")
