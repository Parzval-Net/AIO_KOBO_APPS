"""Audiobook builders: Nickel stock .mp3z and KOReader audiobook.koplugin (pure Python, no ffmpeg for MP3 input).

Output follows what the plugin reads (github.com/stradichenko/audiobook.koplugin, m4bparser.lua):
one MP3 per book with ID3v2.3 chapters (CHAP + CTOC) and an embedded baseline JPEG cover (APIC).
MP3 is the plugin's native playback path on Kobo (no on-device transcoding, lower CPU/battery).
Only for DRM-free audio you own.
"""

from __future__ import annotations

import io
import re
import shutil
import struct
import subprocess
import tempfile
import unicodedata
import zipfile
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageOps

from .core import install_hint

AUDIO_EXTS = {".mp3", ".m4a", ".m4b", ".aac", ".ogg", ".opus", ".flac", ".wav"}
COVER_NAMES = ("cover", "folder", "front", "portada")

# MPEG audio frame tables: bitrate (kbps) per [version][layer][index], sample rate per version
_BR = {
    (1, 1): [0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448],
    (1, 2): [0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384],
    (1, 3): [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320],
    (2, 1): [0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256],
    (2, 2): [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160],
    (2, 3): [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160],
}
_SR = {1: [44100, 48000, 32000], 2: [22050, 24000, 16000], 25: [11025, 12000, 8000]}


def strip_tags(data: bytes) -> bytes:
    """Remove leading ID3v2 and trailing ID3v1/APE tags, leaving raw MPEG frames."""
    start = 0
    while data[start:start + 3] == b"ID3" and len(data) >= start + 10:
        size = (data[start + 6] << 21) | (data[start + 7] << 14) | (data[start + 8] << 7) | data[start + 9]
        footer = 10 if data[start + 5] & 0x10 else 0
        start += 10 + size + footer
    end = len(data)
    if end - start >= 128 and data[end - 128:end - 125] == b"TAG":
        end -= 128
    if data[end - 32:end - 24] == b"APETAGEX":
        ape_size = struct.unpack("<I", data[end - 20:end - 16])[0]
        end -= ape_size + (32 if data[end - ape_size - 32:end - ape_size - 24] == b"APETAGEX" else 0)
    return data[start:end]


def iter_frames(frames: bytes):
    """Yield (start, end, samples, sample_rate) for each MPEG audio frame, resyncing over garbage bytes."""
    i, n = 0, len(frames)
    while i + 4 <= n:
        b1, b2 = frames[i + 1], frames[i + 2]
        if frames[i] != 0xFF or (b1 & 0xE0) != 0xE0:
            i += 1
            continue
        ver_bits, layer_bits = (b1 >> 3) & 3, (b1 >> 1) & 3
        br_idx, sr_idx, pad = (b2 >> 4) & 15, (b2 >> 2) & 3, (b2 >> 1) & 1
        if ver_bits == 1 or layer_bits == 0 or br_idx in (0, 15) or sr_idx == 3:
            i += 1
            continue
        version = {3: 1, 2: 2, 0: 25}[ver_bits]
        layer = 4 - layer_bits
        bitrate = _BR[(1 if version == 1 else 2, layer)][br_idx] * 1000
        rate = _SR[version][sr_idx]
        if layer == 1:
            size, spf = (12 * bitrate // rate + pad) * 4, 384
        else:
            spf = 1152 if (layer == 2 or version == 1) else 576
            size = spf // 8 * bitrate // rate + pad
        if size <= 0:
            i += 1
            continue
        yield i, min(i + size, n), spf, rate
        i += size


def mp3_duration_ms(frames: bytes) -> int:
    """Exact duration by walking MPEG frame headers (works for CBR and VBR)."""
    samples, rate = 0, 0
    for _, _, spf, rate in iter_frames(frames):
        samples += spf
    return int(samples * 1000 / rate) if rate else 0


def _is_info_frame(frame: bytes) -> bool:
    """LAME/Xing 'Info'/'Xing' or Fraunhofer 'VBRI' header frame (carries no audio, describes the whole file)."""
    return any(frame[o:o + 4] in (b"Xing", b"Info") for o in (13, 21, 36)) or frame[36:40] == b"VBRI"


def split_mp3(frames: bytes, max_ms: int = 0, max_bytes: int = 0) -> list[tuple[bytes, int]]:
    """Cut raw MPEG frames into parts of at most max_ms / max_bytes, always on frame boundaries.

    The Xing/Info/VBRI header frame is dropped: it describes the original file, so in a part it would report
    the wrong length. Returns [(frames, duration_ms)]; with no limits, a single part.
    """
    parts, start, cur_ms, cur_samples, rate, first = [], None, 0, 0, 0, True
    for a, b, spf, rate in iter_frames(frames):
        if first:
            first = False
            if _is_info_frame(frames[a:b]):
                continue
        if start is None:
            start, cur_samples = a, 0
        cur_samples += spf
        cur_ms = cur_samples * 1000 // rate
        if (max_ms and cur_ms >= max_ms) or (max_bytes and b - start >= max_bytes):
            parts.append((frames[start:b], cur_ms))
            start = None
    if start is not None:
        parts.append((frames[start:], cur_samples * 1000 // rate if rate else 0))
    if len(parts) > 1 and parts[-1][1] < min(15000, max_ms // 4 or 15000):  # no tiny trailing part: merge it
        (pa, da), (pb, db) = parts[-2], parts[-1]
        parts[-2:] = [(pa + pb, da + db)]
    return parts or [(frames, 0)]


def _syncsafe(n: int) -> bytes:
    return bytes([(n >> 21) & 0x7F, (n >> 14) & 0x7F, (n >> 7) & 0x7F, n & 0x7F])


def _frame(fid: str, payload: bytes) -> bytes:
    return fid.encode("ascii") + struct.pack(">I", len(payload)) + b"\x00\x00" + payload


def _text(fid: str, value: str) -> bytes:
    return _frame(fid, b"\x01" + value.encode("utf-16"))  # UTF-16 with BOM (ID3v2.3)


def build_id3(title: str, author: str, chapters: list[tuple[str, int, int]], cover_jpeg: bytes | None) -> bytes:
    frames = [_text("TIT2", title), _text("TALB", title), _text("TPE1", author or "Desconocido"),
              _text("TCON", "Audiobook")]
    if cover_jpeg:
        frames.append(_frame("APIC", b"\x00" + b"image/jpeg\x00" + b"\x03" + b"Cover\x00" + cover_jpeg))
    if chapters:
        ids = [f"ch{i:03d}".encode() for i in range(len(chapters))]
        frames.append(_frame("CTOC", b"toc\x00" + b"\x03" + bytes([len(ids)]) + b"".join(c + b"\x00" for c in ids)))
        for cid, (name, start, end) in zip(ids, chapters):
            sub = _text("TIT2", name)
            frames.append(_frame("CHAP", cid + b"\x00" + struct.pack(">IIII", start, end, 0xFFFFFFFF, 0xFFFFFFFF) + sub))
    body = b"".join(frames)
    return b"ID3\x03\x00\x00" + _syncsafe(len(body)) + body


def prepare_cover(path_or_bytes, max_side: int = 1000) -> bytes:
    """Baseline JPEG <= max_side px (what the plugin's cover cache expects)."""
    img = Image.open(io.BytesIO(path_or_bytes) if isinstance(path_or_bytes, bytes) else path_or_bytes)
    img = ImageOps.exif_transpose(img).convert("RGB")
    img.thumbnail((max_side, max_side), Image.LANCZOS)
    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=88, optimize=True, progressive=False)
    return buf.getvalue()


def embedded_cover(mp3: bytes) -> bytes | None:
    """Extract an APIC picture from an ID3v2.3/2.4 tag, if any."""
    if mp3[:3] != b"ID3":
        return None
    ver = mp3[3]
    size = (mp3[6] << 21) | (mp3[7] << 14) | (mp3[8] << 7) | mp3[9]
    tag, i = mp3[10:10 + size], 0
    while i + 10 <= len(tag):
        fid = tag[i:i + 4]
        fsz = (int.from_bytes(tag[i + 4:i + 8], "big") if ver == 3 else
               (tag[i + 4] << 21) | (tag[i + 5] << 14) | (tag[i + 6] << 7) | tag[i + 7])
        if not fid.strip(b"\x00") or fsz <= 0:
            break
        if fid == b"APIC":
            p = tag[i + 10:i + 10 + fsz]
            mime_end = p.index(b"\x00", 1)
            j = mime_end + 2
            enc = p[0]
            if enc in (1, 2):
                while j + 1 < len(p) and p[j:j + 2] != b"\x00\x00":
                    j += 2
                j += 2
            else:
                j = p.index(b"\x00", j) + 1
            return p[j:]
        i += 10 + fsz
    return None


@dataclass
class AudiobookInput:
    title: str
    author: str
    tracks: list[Path]
    cover: Path | None = None


def natural_key(s: str):
    return [int(t) if t.isdigit() else t.lower() for t in re.split(r"(\d+)", s)]


def collect(src: Path, title: str = "", author: str = "", cover: Path | None = None) -> AudiobookInput:
    src = Path(src)
    if src.is_dir():
        tracks = sorted((p for p in src.rglob("*") if p.suffix.lower() in AUDIO_EXTS and not p.name.startswith(".")),
                        key=lambda p: natural_key(str(p.relative_to(src))))
        if cover is None:
            imgs = [p for p in src.rglob("*") if p.suffix.lower() in (".jpg", ".jpeg", ".png", ".webp")
                    and not p.name.startswith(".")]
            imgs.sort(key=lambda p: (0 if p.stem.lower() in COVER_NAMES else 1, -p.stat().st_size))
            cover = imgs[0] if imgs else None
        guess = src.name
    else:
        tracks, guess = [src], src.stem
    if not tracks:
        raise RuntimeError(f"No hay archivos de audio en {src.name}")
    if not title and " - " in guess and not author:
        author, title = [x.strip() for x in guess.split(" - ", 1)]
    return AudiobookInput(title or guess, author, tracks, cover)


def _chapter_name(p: Path, idx: int) -> str:
    name = re.sub(r"^[\d\s._-]+", "", p.stem).strip()
    return name or f"Capítulo {idx}"


def build(book: AudiobookInput, progress=lambda m: None) -> tuple[str, bytes, str]:
    """Return (filename, data, format). MP3 inputs are joined natively; others need ffmpeg (-> M4B)."""
    exts = {t.suffix.lower() for t in book.tracks}
    cover = prepare_cover(book.cover) if book.cover else None
    safe = re.sub(r"[\\/:*?\"<>|]+", " ", book.title).strip() or "Audiolibro"
    if exts == {".mp3"}:
        parts, chapters, pos = [], [], 0
        for idx, t in enumerate(book.tracks, 1):
            raw = t.read_bytes()
            if cover is None:
                emb = embedded_cover(raw)
                if emb:
                    try:
                        cover = prepare_cover(emb)
                    except Exception:  # noqa: BLE001 - broken embedded art: ignore
                        pass
            frames = strip_tags(raw)
            dur = mp3_duration_ms(frames)
            chapters.append((_chapter_name(t, idx) if len(book.tracks) > 1 else book.title, pos, pos + dur))
            pos += dur
            parts.append(frames)
            progress(f"Pista {idx}/{len(book.tracks)}: {t.name} ({dur // 60000} min)")
        return f"{safe}.mp3", build_id3(book.title, book.author, chapters, cover) + b"".join(parts), "mp3"
    if len(book.tracks) == 1 and exts <= {".m4b"} and not cover:
        return f"{safe}.m4b", book.tracks[0].read_bytes(), "m4b"
    return _build_ffmpeg(book, safe, cover, progress)


def ascii_name(text: str, fallback: str = "Pista") -> str:
    """ASCII-only file name. Nickel's libmp3z.so looks entries up with QString::toLatin1() while libzip stores
    UTF-8 names, so any accent (or other non-ASCII char) makes the track unplayable."""
    t = unicodedata.normalize("NFKD", text).encode("ascii", "ignore").decode("ascii")
    t = re.sub(r'[\\/:*?"<>|\x00-\x1f]+', " ", t)
    return re.sub(r"\s+", " ", t).strip(" .")[:80] or fallback


PART_MAX_BYTES = 48 * 1024 * 1024   # each track is loaded whole into RAM by Nickel (QByteArray + QBuffer)
FAT32_MAX = 4 * 1024**3 - 1


def build_mp3z(book: AudiobookInput, progress=lambda m: None, part_minutes: int = 10) -> tuple[str, bytes, str]:
    """Nickel's stock audiobook format (.mp3z, mime application/x-kobo-mp3z), shaped to how firmware 4.45's
    libmp3z.so really reads it (REA/Ghidra on /usr/local/Kobo/libmp3z.so):

    - MP3ReaderParser::parse lists every ZIP entry, sorts names with QString '<' (case sensitive) and makes each
      one a chapter titled "Page N". Book title = file name without extension; author/description stay empty and
      parseCoverImage() returns nothing (generic library tile). So: only MP3 entries, zero padded ASCII names.
    - MP3Archive::data reads the entry whole into a QByteArray, looked up with toLatin1() -> ASCII names only.
    - MP3Reader::saveBookmark stores only the track index (no position inside a track) and marks a single track
      book as finished. So long chapters are cut into parts of `part_minutes` (0 = keep tracks as they are):
      reopening resumes at most that far back, and memory per track stays bounded.
    - AudioPlayer shows QMediaMetaData::CoverArtImage, so every track carries the cover (ID3v2.3 APIC).
    """
    exts = {t.suffix.lower() for t in book.tracks}
    if exts != {".mp3"}:
        raise RuntimeError("El formato stock de Nickel (.mp3z) solo admite pistas MP3")
    cover = prepare_cover(book.cover, 600) if book.cover else None
    if cover is None:
        for t in book.tracks:
            emb = embedded_cover(t.read_bytes())
            if emb:
                try:
                    cover = prepare_cover(emb, 600)
                    break
                except Exception:  # noqa: BLE001
                    pass
    safe = re.sub(r"[\\/:*?\"<>|]+", " ", book.title).strip() or "Audiolibro"
    max_ms = max(0, int(part_minutes or 0)) * 60000
    chapters: list[tuple[str, list[tuple[bytes, int]]]] = []
    for idx, t in enumerate(book.tracks, 1):
        parts = split_mp3(strip_tags(t.read_bytes()), max_ms, PART_MAX_BYTES)
        chapters.append((_chapter_name(t, idx) if len(book.tracks) > 1 else book.title, parts))
        progress(f"Pista {idx}/{len(book.tracks)}: {t.name} ({len(parts)} parte(s))")
    total = sum(len(p) for _, p in chapters)
    width = max(3, len(str(total)))
    buf = io.BytesIO()
    n = 0
    with zipfile.ZipFile(buf, "w", compression=zipfile.ZIP_STORED, allowZip64=True) as zf:
        for chap, parts in chapters:
            for k, (frames, _dur) in enumerate(parts, 1):
                n += 1
                label = chap if len(parts) == 1 else f"{chap} ({k}-{len(parts)})"
                tag = [_text("TIT2", label), _text("TALB", book.title), _text("TPE1", book.author or "Desconocido"),
                       _text("TRCK", f"{n}/{total}"), _text("TCON", "Audiobook")]
                if cover:
                    tag.append(_frame("APIC", b"\x00image/jpeg\x00\x03Cover\x00" + cover))
                body = b"".join(tag)
                info = zipfile.ZipInfo(f"{n:0{width}d} - {ascii_name(label)}.mp3", date_time=(2020, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_STORED
                zf.writestr(info, b"ID3\x03\x00\x00" + _syncsafe(len(body)) + body + frames)
    data = buf.getvalue()
    if len(data) > FAT32_MAX:
        raise RuntimeError("El audiolibro supera 4 GB (límite de archivo del Kobo). Divídelo en dos libros.")
    return f"{safe}.mp3z", data, "mp3z"


def _build_ffmpeg(book: AudiobookInput, safe: str, cover: bytes | None, progress) -> tuple[str, bytes, str]:
    ffmpeg, ffprobe = shutil.which("ffmpeg"), shutil.which("ffprobe")
    if not ffmpeg or not ffprobe:
        raise RuntimeError(f"Para formatos distintos de MP3 hace falta ffmpeg: {install_hint('ffmpeg')}")
    with tempfile.TemporaryDirectory() as tmp:
        tmpd = Path(tmp)
        lst = tmpd / "list.txt"
        lst.write_text("".join(f"file '{str(t).replace(chr(39), chr(39) + chr(92) + chr(39) + chr(39))}'\n"
                               for t in book.tracks), encoding="utf-8")
        meta = [";FFMETADATA1", f"title={book.title}", f"artist={book.author}", f"album={book.title}", "genre=Audiobook"]
        pos = 0
        for idx, t in enumerate(book.tracks, 1):
            out = subprocess.run([ffprobe, "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", str(t)],
                                 capture_output=True, text=True, check=True).stdout.strip()
            dur = int(float(out) * 1000)
            meta += ["[CHAPTER]", "TIMEBASE=1/1000", f"START={pos}", f"END={pos + dur}", f"title={_chapter_name(t, idx)}"]
            pos += dur
        (tmpd / "meta.txt").write_text("\n".join(meta) + "\n", encoding="utf-8")
        cmd = [ffmpeg, "-y", "-v", "error", "-f", "concat", "-safe", "0", "-i", str(lst), "-i", str(tmpd / "meta.txt")]
        if cover:
            (tmpd / "cover.jpg").write_bytes(cover)
            cmd += ["-i", str(tmpd / "cover.jpg"), "-map", "0:a", "-map", "2:v", "-disposition:v", "attached_pic", "-c:v", "copy"]
        else:
            cmd += ["-map", "0:a"]
        out = tmpd / "out.m4b"
        cmd += ["-map_metadata", "1", "-map_chapters", "1", "-c:a", "aac", "-b:a", "64k", "-movflags", "+faststart", str(out)]
        progress("Codificando M4B con ffmpeg…")
        subprocess.run(cmd, check=True, capture_output=True)
        return f"{safe}.m4b", out.read_bytes(), "m4b"
