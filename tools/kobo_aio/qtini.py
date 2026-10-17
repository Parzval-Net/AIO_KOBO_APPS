"""Minimal-diff editor for Qt QSettings INI files such as '.kobo/Kobo/Kobo eReader.conf'.

Nickel reads this file with QSettings(IniFormat). In that format a literal backslash in a value
must be written as two backslashes; a single backslash followed by '.' is dropped by Qt when Nickel
re-saves the file, which turns ExcludeSyncFolders into a regex that hides every sideloaded folder.
This module only rewrites the lines it is asked to change and leaves everything else byte-identical.
"""

from __future__ import annotations

import re

_NEEDS_QUOTES = re.compile(r'[;,"=]|^\s|\s$')


def qt_escape(value: str) -> str:
    """Encode a plain string as a QSettings INI value."""
    out = value.replace("\\", "\\\\")
    if _NEEDS_QUOTES.search(value):
        out = '"' + out.replace('"', '\\"') + '"'
    return out


def qt_unescape(raw: str) -> str:
    """Decode a QSettings INI value (subset: quotes, \\\\, \\", \\n, \\t)."""
    raw = raw.strip()
    quoted = len(raw) >= 2 and raw[0] == '"' and raw[-1] == '"'
    if quoted:
        raw = raw[1:-1]
    out = []
    i = 0
    while i < len(raw):
        c = raw[i]
        if c == "\\" and i + 1 < len(raw):
            n = raw[i + 1]
            out.append({"\\": "\\", '"': '"', "n": "\n", "t": "\t", "r": "\r"}.get(n, n))
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def _qt_key(key: str) -> str:
    # QSettings percent-encodes some characters in keys; we only handle plain ASCII keys here.
    return key


class QtIni:
    def __init__(self, text: str = ""):
        self.newline = "\r\n" if "\r\n" in text else "\n"
        self.lines = text.splitlines()
        self.trailing_newline = text.endswith(("\n", "\r\n")) or not text

    @classmethod
    def load(cls, path) -> "QtIni":
        try:
            with open(path, "r", encoding="utf-8", errors="surrogateescape", newline="") as fh:
                return cls(fh.read())
        except FileNotFoundError:
            return cls("")

    def dumps(self) -> str:
        body = self.newline.join(self.lines)
        return body + (self.newline if self.trailing_newline and self.lines else "")

    def sections(self) -> list[str]:
        return [l.strip()[1:-1] for l in self.lines if self._is_section(l)]

    @staticmethod
    def _is_section(line: str) -> bool:
        s = line.strip()
        return s.startswith("[") and s.endswith("]")

    def _find(self, section: str, key: str) -> tuple[int, int, int]:
        """Return (section_header_idx, key_idx, section_end_idx); -1 when missing."""
        sec_idx = -1
        key_idx = -1
        end_idx = len(self.lines)
        for i, line in enumerate(self.lines):
            if self._is_section(line):
                if sec_idx >= 0:
                    end_idx = i
                    break
                if line.strip()[1:-1] == section:
                    sec_idx = i
                continue
            if sec_idx >= 0 and key_idx < 0:
                k, sep, _ = line.partition("=")
                if sep and k.strip() == _qt_key(key):
                    key_idx = i
        return sec_idx, key_idx, end_idx

    def get_raw(self, section: str, key: str) -> str | None:
        _, k, _ = self._find(section, key)
        return None if k < 0 else self.lines[k].partition("=")[2]

    def get(self, section: str, key: str) -> str | None:
        raw = self.get_raw(section, key)
        return None if raw is None else qt_unescape(raw)

    def set(self, section: str, key: str, value: str) -> bool:
        """Set a plain-string value. Returns True if the file content changed."""
        line = f"{_qt_key(key)}={qt_escape(value)}"
        sec, k, end = self._find(section, key)
        if k >= 0:
            if self.lines[k] == line:
                return False
            self.lines[k] = line
            return True
        if sec < 0:
            if self.lines and self.lines[-1].strip():
                self.lines.append("")
            self.lines += [f"[{section}]", line]
            return True
        # Insert before trailing blank lines of the section
        insert_at = end
        while insert_at - 1 > sec and not self.lines[insert_at - 1].strip():
            insert_at -= 1
        self.lines.insert(insert_at, line)
        return True

    def remove(self, section: str, key: str) -> bool:
        _, k, _ = self._find(section, key)
        if k < 0:
            return False
        del self.lines[k]
        return True
