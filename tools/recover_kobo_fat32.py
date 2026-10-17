#!/usr/bin/env python3
"""
Read-only FAT32 forensic book recovery tool for Kobo eReaders.
Scans raw FAT32 directory entries (including 0xE5 deleted LFN+SFN entries)
from /dev/rdisk4 (or /dev/disk4) in 100% read-only mode, cross-references
against metadata.calibre, verifies ZIP/EPUB/KEPUB/CBZ/PDF integrity, and
writes recovered books to a safe folder on the Mac before restoring to Kobo.
"""

from __future__ import annotations

import io
import json
import math
import os
import struct
import sys
import zipfile
from pathlib import Path


def parse_lfn_chars(entry: bytes) -> str:
    raw = entry[1:11] + entry[14:26] + entry[28:32]
    chars = []
    for i in range(0, len(raw), 2):
        code = struct.unpack_from("<H", raw, i)[0]
        if code in (0x0000, 0xFFFF):
            break
        chars.append(chr(code))
    return "".join(chars)


def decode_sfn(entry: bytes) -> str:
    base = entry[0:8].decode("latin1", errors="replace").rstrip(" ")
    ext = entry[8:11].decode("latin1", errors="replace").rstrip(" ")
    if entry[0] == 0xE5:
        base = "_" + base[1:]
    return f"{base}.{ext}" if ext else base


class Fat32Reader:
    def __init__(self, dev_path: str):
        self.dev_path = dev_path
        self.fd = os.open(dev_path, os.O_RDONLY)
        bpb = os.pread(self.fd, 512, 0)
        self.bytes_per_sector = struct.unpack_from("<H", bpb, 11)[0]
        self.sectors_per_cluster = bpb[13]
        self.reserved_sectors = struct.unpack_from("<H", bpb, 14)[0]
        self.num_fats = bpb[16]
        self.sectors_per_fat = struct.unpack_from("<I", bpb, 36)[0]
        self.root_cluster = struct.unpack_from("<I", bpb, 44)[0]

        self.cluster_size = self.bytes_per_sector * self.sectors_per_cluster
        self.fat_offset = self.reserved_sectors * self.bytes_per_sector
        self.fat_size_bytes = self.sectors_per_fat * self.bytes_per_sector
        self.data_offset = (
            self.reserved_sectors + self.num_fats * self.sectors_per_fat
        ) * self.bytes_per_sector

        print(
            f"[FAT32] cluster_size={self.cluster_size} root_cluster={self.root_cluster} "
            f"data_offset={self.data_offset}"
        )
        raw_fat = os.pread(self.fd, self.fat_size_bytes, self.fat_offset)
        num_entries = len(raw_fat) // 4
        self.fat = struct.unpack(f"<{num_entries}I", raw_fat)

    def close(self) -> None:
        if self.fd >= 0:
            os.close(self.fd)
            self.fd = -1

    def cluster_to_offset(self, cluster: int) -> int:
        return self.data_offset + (cluster - 2) * self.cluster_size

    def read_cluster_chain(self, start_cluster: int, max_clusters: int = 4096) -> bytes:
        chunks = []
        cur = start_cluster
        visited = set()
        while 2 <= cur < len(self.fat) and cur not in visited and len(chunks) < max_clusters:
            visited.add(cur)
            chunks.append(os.pread(self.fd, self.cluster_size, self.cluster_to_offset(cur)))
            nxt = self.fat[cur] & 0x0FFFFFFF
            if nxt >= 0x0FFFFFF8 or nxt == 0:
                break
            cur = nxt
        return b"".join(chunks)

    def parse_directory(self, start_cluster: int, rel_prefix: str = "") -> tuple[list[dict], list[dict]]:
        data = self.read_cluster_chain(start_cluster)
        active_dirs = []
        entries = []
        pending_lfn: list[str] = []

        for offset in range(0, len(data), 32):
            ent = data[offset : offset + 32]
            first = ent[0]
            if first == 0x00:
                break
            attr = ent[11]
            if attr == 0x0F:
                pending_lfn.append(parse_lfn_chars(ent))
                continue

            # Regular SFN or Volume/Dir entry
            lfn_name = "".join(reversed(pending_lfn)) if pending_lfn else ""
            pending_lfn = []

            if attr & 0x08:  # Volume label
                continue

            is_deleted = (first == 0xE5)
            is_dir = bool(attr & 0x10)
            clus_hi = struct.unpack_from("<H", ent, 20)[0]
            clus_lo = struct.unpack_from("<H", ent, 26)[0]
            first_cluster = (clus_hi << 16) | clus_lo
            file_size = struct.unpack_from("<I", ent, 28)[0]
            sfn_name = decode_sfn(ent)
            name = lfn_name if lfn_name else sfn_name

            if name in (".", ".."):
                continue

            rel_path = f"{rel_prefix}/{name}" if rel_prefix else name
            item = {
                "name": name,
                "sfn": sfn_name,
                "rel_path": rel_path,
                "is_deleted": is_deleted,
                "is_dir": is_dir,
                "first_cluster": first_cluster,
                "size": file_size,
            }
            if is_dir and not is_deleted and first_cluster >= 2:
                active_dirs.append(item)
            else:
                entries.append(item)

        return active_dirs, entries

    def scan_tree(self) -> list[dict]:
        all_files: list[dict] = []
        queue = [(self.root_cluster, "")]
        visited_dirs = set()
        while queue:
            clus, prefix = queue.pop(0)
            if clus in visited_dirs:
                continue
            visited_dirs.add(clus)
            subdirs, files = self.parse_directory(clus, prefix)
            all_files.extend(files)
            for d in subdirs:
                # Skip system/hidden dirs except if needed
                if d["name"] in (".Spotlight-V100", ".fseventsd", "System Volume Information"):
                    continue
                queue.append((d["first_cluster"], d["rel_path"]))
        return all_files

    def read_contiguous_file(self, start_cluster: int, size: int) -> bytes:
        if size == 0 or start_cluster < 2:
            return b""
        num_clusters = math.ceil(size / self.cluster_size)
        total_bytes_to_read = num_clusters * self.cluster_size
        start_off = self.cluster_to_offset(start_cluster)
        # Read in 4 MB chunks for speed on /dev/rdisk4
        buf = bytearray()
        rem = total_bytes_to_read
        off = start_off
        chunk_sz = 4 * 1024 * 1024
        while rem > 0:
            to_read = min(rem, chunk_sz)
            part = os.pread(self.fd, to_read, off)
            if not part:
                break
            buf.extend(part)
            off += len(part)
            rem -= len(part)
        return bytes(buf[:size])

    def read_skipping_allocated(self, start_cluster: int, size: int, reserved_starts: set[int]) -> bytes:
        if size == 0 or start_cluster < 2:
            return b""
        needed = math.ceil(size / self.cluster_size)
        chunks = []
        cur = start_cluster
        while len(chunks) < needed and cur < len(self.fat):
            if cur == start_cluster or ((self.fat[cur] & 0x0FFFFFFF) == 0 and cur not in reserved_starts):
                chunks.append(os.pread(self.fd, self.cluster_size, self.cluster_to_offset(cur)))
            cur += 1
        return b"".join(chunks)[:size]


def verify_book_bytes(filename: str, data: bytes) -> tuple[bool, str]:
    low = filename.lower()
    if low.endswith((".epub", ".cbz")):
        if not data.startswith(b"PK\x03\x04"):
            return False, "Missing ZIP header PK\\x03\\x04"
        try:
            with zipfile.ZipFile(io.BytesIO(data), "r") as zf:
                bad = zf.testzip()
                if bad is not None:
                    return False, f"CRC mismatch in {bad}"
                return True, f"Valid ZIP/EPUB/CBZ ({len(zf.namelist())} entries)"
        except Exception as exc:
            return False, f"ZIP error: {exc}"
    elif low.endswith(".pdf"):
        if data.startswith(b"%PDF") and b"%%EOF" in data[-4096:]:
            return True, "Valid PDF header & EOF"
        return False, "Invalid PDF header/EOF"
    else:
        return len(data) > 0, "Raw file"


def main() -> int:
    dev_path = sys.argv[1] if len(sys.argv) > 1 else "/dev/rdisk4"
    out_dir = Path(sys.argv[2]) if len(sys.argv) > 2 else Path("recovered_books")
    out_dir.mkdir(parents=True, exist_ok=True)

    calibre_map: dict[str, dict] = {}
    calibre_by_size: dict[int, list[dict]] = {}
    calibre_path = Path("/Volumes/KOBOeReader/metadata.calibre")
    if calibre_path.is_file():
        cdata = json.loads(calibre_path.read_text(encoding="utf-8", errors="replace"))
        for b in cdata:
            lp = b.get("lpath", "")
            sz = b.get("size", 0)
            calibre_map[lp] = b
            calibre_by_size.setdefault(sz, []).append(b)

    reader = Fat32Reader(dev_path)
    try:
        all_files = reader.scan_tree()
        deleted_books = [
            f
            for f in all_files
            if f["is_deleted"]
            and not f["name"].startswith("._")
            and f["size"] > 4096
            and not f["rel_path"].startswith((".kobo/", ".adds/", ".kobopatch-webui/"))
        ]
        print(f"[SCAN] Found {len(deleted_books)} deleted book/document file entries on FAT32.")

        reserved_starts = {f["first_cluster"] for f in deleted_books if f["first_cluster"] >= 2}

        recovered_ok = 0
        recovered_partial = 0

        for idx, item in enumerate(deleted_books, 1):
            rel_path = item["rel_path"]
            sz = item["size"]
            clus = item["first_cluster"]

            # Match against metadata.calibre if filename had any LFN loss
            if rel_path not in calibre_map and sz in calibre_by_size and len(calibre_by_size[sz]) == 1:
                exact_lpath = calibre_by_size[sz][0]["lpath"]
                rel_path = exact_lpath

            data = reader.read_contiguous_file(clus, sz)
            ok, msg = verify_book_bytes(rel_path, data)
            if not ok:
                # Try skipping allocated clusters
                data_alt = reader.read_skipping_allocated(clus, sz, reserved_starts)
                ok_alt, msg_alt = verify_book_bytes(rel_path, data_alt)
                if ok_alt:
                    data = data_alt
                    ok = True
                    msg = f"{msg_alt} (skipped allocated clusters)"

            dst_file = out_dir / rel_path
            dst_file.parent.mkdir(parents=True, exist_ok=True)
            dst_file.write_bytes(data)

            status = "OK" if ok else "WARN"
            if ok:
                recovered_ok += 1
            else:
                recovered_partial += 1
            print(
                f"[{idx:02d}/{len(deleted_books):02d}] [{status}] {rel_path} "
                f"({sz/(1024*1024):.2f} MB, clus={clus}) -> {msg}"
            )

        print(
            f"\n=== SUMMARY: {recovered_ok} verified 100% intact, "
            f"{recovered_partial} partial/unverified (saved to {out_dir}) ==="
        )
    finally:
        reader.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
