#!/usr/bin/env python3
"""Minimal LSPROG progress-sync server for the LangStudy firmware.

The device POSTs its local progress snapshot as ``text/plain`` and receives the
merged snapshot back. The server keeps one authoritative snapshot per URL path,
so ``--sync-url http://host:8080/devices/alice`` gives each device (or family
member) an independent history.

Merge rules mirror ``ls_progress_merge()`` in ``main/langstudy_progress.c``:

* a per-word record wins when its ``revision`` is higher; ties keep the server
  copy;
* a new word is appended while the table has room;
* ``session_count`` and the counters follow whichever side has more completed
  sessions.

The wire format and CRC are exactly ``LSPROG`` v1 as written by
``ls_progress_write()``. The server is dependency-free (standard library only)
and stores snapshots as plain files under ``--store``.

Usage::

    python3 tools/langstudy_sync_server.py --port 8080 --store tools/var/sync

Then configure ``http://<host>:8080/<path>`` as the device sync URL.
"""

from __future__ import annotations

import argparse
import copy
import os
import re
import sys
import tempfile
import threading
import zlib
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

FORMAT_VERSION = 1
RECORDS_MAX = 256
PROGRESS_TEXT_MAX = 20480
LINE_MAX_BYTES = 160
U16_MAX = 0xFFFF
U32_MAX = 0xFFFFFFFF
KEY_MAX = 256
SAFE_KEY_RE = re.compile(r"[A-Za-z0-9._/-]+")
HEX32_RE = re.compile(r"[0-9A-Fa-f]{8}")


class ProgressError(ValueError):
    """Raised when a snapshot is malformed, out of range or fails its CRC."""


@dataclass
class Record:
    word_id: int
    box: int
    correct: int
    wrong: int
    revision: int
    due_session: int


@dataclass
class Progress:
    session_count: int = 0
    answered_total: int = 0
    correct_total: int = 0
    records: list[Record] = field(default_factory=list)


def _parse_u32(field_text: bytes) -> int:
    if not 1 <= len(field_text) <= 10 or not field_text.isdigit():
        raise ProgressError("invalid unsigned integer field")
    value = int(field_text)
    if value > U32_MAX:
        raise ProgressError("integer field out of range")
    return value


def _fields(line: bytes, expected: int) -> list[bytes]:
    parts = line.split(b"|")
    if len(parts) != expected:
        raise ProgressError(f"expected {expected} fields, got {len(parts)}")
    return parts


def parse_progress(data: bytes) -> Progress:
    """Parse and validate one ``LSPROG`` v1 snapshot."""
    if not isinstance(data, (bytes, bytearray)):
        raise ProgressError("snapshot must be bytes")
    if not 2 <= len(data) <= PROGRESS_TEXT_MAX:
        raise ProgressError("snapshot length out of range")
    if data[-1:] != b"\n" or b"\0" in data or b"\r" in data:
        raise ProgressError("snapshot must be LF-terminated without CR/NUL")

    lines = data.split(b"\n")
    if lines[-1] != b"":
        raise ProgressError("snapshot must end with a newline")
    lines = lines[:-1]
    if not lines:
        raise ProgressError("empty snapshot")

    for line in lines:
        if len(line) > LINE_MAX_BYTES:
            raise ProgressError("line too long")

    header = _fields(lines[0], 6)
    if header[0] != b"LSPROG" or _parse_u32(header[1]) != FORMAT_VERSION:
        raise ProgressError("unsupported snapshot header")
    progress = Progress(
        session_count=_parse_u32(header[2]),
        answered_total=_parse_u32(header[3]),
        correct_total=_parse_u32(header[4]),
    )
    expected_records = _parse_u32(header[5])
    if expected_records > RECORDS_MAX:
        raise ProgressError("record count out of range")
    if progress.correct_total > progress.answered_total:
        raise ProgressError("correct total exceeds answered total")

    if len(lines) < 2 or not lines[-1].startswith(b"END|"):
        raise ProgressError("missing END line")
    end = _fields(lines[-1], 2)
    if end[0] != b"END" or not HEX32_RE.fullmatch(end[1].decode("ascii", "replace")):
        raise ProgressError("invalid END line")
    expected_crc = int(end[1], 16)

    frame = b"\n".join(lines[:-1]) + b"\n"
    if zlib.crc32(frame) & U32_MAX != expected_crc:
        raise ProgressError("checksum mismatch")

    seen: set[int] = set()
    for line in lines[1:-1]:
        parts = _fields(line, 7)
        if parts[0] != b"WORD":
            raise ProgressError("unknown record type")
        record = Record(
            word_id=_parse_u32(parts[1]),
            box=_parse_u32(parts[2]),
            correct=_parse_u32(parts[3]),
            wrong=_parse_u32(parts[4]),
            revision=_parse_u32(parts[5]),
            due_session=_parse_u32(parts[6]),
        )
        if record.word_id == 0 or record.box > 4:
            raise ProgressError("word id or box out of range")
        if record.correct > U16_MAX or record.wrong > U16_MAX:
            raise ProgressError("answer counters out of range")
        if record.word_id in seen:
            raise ProgressError("duplicate word id")
        seen.add(record.word_id)
        progress.records.append(record)

    if len(progress.records) != expected_records:
        raise ProgressError("record count does not match header")
    return progress


def write_progress(progress: Progress) -> bytes:
    """Serialize ``progress`` as ``LSPROG`` v1 with a fresh CRC."""
    if len(progress.records) > RECORDS_MAX:
        raise ProgressError("too many records")
    if progress.correct_total > progress.answered_total:
        raise ProgressError("correct total exceeds answered total")

    lines = [
        "LSPROG|%d|%d|%d|%d|%d"
        % (
            FORMAT_VERSION,
            progress.session_count,
            progress.answered_total,
            progress.correct_total,
            len(progress.records),
        )
    ]
    seen: set[int] = set()
    for record in progress.records:
        if not 0 < record.word_id <= U32_MAX or record.word_id in seen:
            raise ProgressError("invalid or duplicate word id")
        if not 0 <= record.box <= 4:
            raise ProgressError("box out of range")
        if not 0 <= record.correct <= U16_MAX or not 0 <= record.wrong <= U16_MAX:
            raise ProgressError("answer counters out of range")
        if not 0 <= record.revision <= U32_MAX:
            raise ProgressError("revision out of range")
        if not 0 <= record.due_session <= U32_MAX:
            raise ProgressError("due session out of range")
        seen.add(record.word_id)
        lines.append(
            "WORD|%d|%d|%d|%d|%d|%d"
            % (
                record.word_id,
                record.box,
                record.correct,
                record.wrong,
                record.revision,
                record.due_session,
            )
        )

    frame = ("\n".join(lines) + "\n").encode("ascii")
    if len(frame) > PROGRESS_TEXT_MAX:
        raise ProgressError("snapshot too large")
    return frame + ("END|%08X\n" % (zlib.crc32(frame) & U32_MAX)).encode("ascii")


def merge_progress(stored: Progress, incoming: Progress) -> Progress:
    """Merge ``incoming`` into the authoritative ``stored`` snapshot."""
    merged = copy.deepcopy(stored)
    by_id = {record.word_id: index for index, record in enumerate(merged.records)}

    for record in incoming.records:
        index = by_id.get(record.word_id)
        if index is not None:
            if record.revision > merged.records[index].revision:
                merged.records[index] = copy.deepcopy(record)
        elif len(merged.records) < RECORDS_MAX:
            by_id[record.word_id] = len(merged.records)
            merged.records.append(copy.deepcopy(record))

    if incoming.session_count > stored.session_count:
        merged.session_count = incoming.session_count
        merged.answered_total = incoming.answered_total
        merged.correct_total = incoming.correct_total
    else:
        if merged.correct_total > merged.answered_total:
            merged.correct_total = merged.answered_total
    return merged


def store_key(request_path: str) -> str:
    """Map a request path to a safe, collision-free snapshot file name."""
    key = request_path.split("?", 1)[0].strip("/")
    if not key:
        key = "default"
    if len(key) > KEY_MAX:
        raise ProgressError("sync path too long")
    parts = key.split("/")
    if any(part in ("", ".", "..") for part in parts):
        raise ProgressError("invalid sync path")
    if not SAFE_KEY_RE.fullmatch(key):
        raise ProgressError("sync path contains unsupported characters")
    return key.replace("/", "__") + ".lsprog"


class SnapshotStore:
    """Per-path snapshot storage with atomic replacement."""

    def __init__(self, root: Path) -> None:
        self.root = root
        self.root.mkdir(parents=True, exist_ok=True)
        self.lock = threading.Lock()

    def path_for(self, request_path: str) -> Path:
        return self.root / store_key(request_path)

    def load(self, request_path: str) -> Progress | None:
        path = self.path_for(request_path)
        if not path.is_file():
            return None
        return parse_progress(path.read_bytes())

    def save(self, request_path: str, progress: Progress) -> None:
        path = self.path_for(request_path)
        data = write_progress(progress)
        with tempfile.NamedTemporaryFile(
            "wb", dir=self.root, prefix=path.name + ".", delete=False
        ) as handle:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
            temp_path = Path(handle.name)
        os.replace(temp_path, path)

    def exchange(self, request_path: str, incoming: Progress) -> Progress:
        """Merge ``incoming`` into storage and return the merged snapshot."""
        with self.lock:
            stored = self.load(request_path)
            merged = incoming if stored is None else merge_progress(stored, incoming)
            self.save(request_path, merged)
            return merged


class SyncHTTPServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address: tuple[str, int], store: SnapshotStore) -> None:
        super().__init__(address, SyncRequestHandler)
        self.store = store


class SyncRequestHandler(BaseHTTPRequestHandler):
    server_version = "LangStudySync/1"
    protocol_version = "HTTP/1.1"

    @property
    def store(self) -> SnapshotStore:
        return self.server.store  # type: ignore[attr-defined]

    def log_message(self, fmt: str, *args: object) -> None:
        sys.stderr.write(
            "%s - - [%s] %s\n"
            % (self.address_string(), self.log_date_time_string(), fmt % args)
        )

    def _respond(self, status: int, body: bytes, content_type: str) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _error(self, status: int, message: str) -> None:
        self._respond(status, (message + "\n").encode("utf-8"), "text/plain; charset=utf-8")

    def _request_path(self) -> str:
        return self.path.split("?", 1)[0]

    def do_GET(self) -> None:  # noqa: N802 - required by BaseHTTPRequestHandler
        path = self._request_path()
        if path == "/health":
            self._respond(200, b"ok\n", "text/plain")
            return
        try:
            progress = self.store.load(path)
        except (ProgressError, OSError) as exc:
            self._error(500, "stored snapshot is unreadable: %s" % exc)
            return
        if progress is None:
            self._error(404, "no snapshot for this path")
            return
        self._respond(200, write_progress(progress), "text/plain")

    def do_POST(self) -> None:  # noqa: N802 - required by BaseHTTPRequestHandler
        try:
            length = int(self.headers.get("Content-Length", ""))
        except ValueError:
            self._error(411, "Content-Length is required")
            return
        if length < 0 or length > PROGRESS_TEXT_MAX:
            self._error(413, "snapshot too large")
            return

        data = self.rfile.read(length)
        if len(data) != length:
            self._error(400, "incomplete request body")
            return

        try:
            incoming = parse_progress(data)
            merged = self.store.exchange(self._request_path(), incoming)
        except ProgressError as exc:
            self._error(400, "invalid snapshot: %s" % exc)
            return
        except OSError as exc:
            self._error(500, "storage failure: %s" % exc)
            return

        self._respond(200, write_progress(merged), "text/plain")


def create_server(host: str, port: int, store_dir: Path) -> SyncHTTPServer:
    return SyncHTTPServer((host, port), SnapshotStore(store_dir))


def load_dotenv(root: Path | None = None) -> None:
    """Load KEY=VALUE lines from .env files without overriding the environment."""
    root = root or Path(__file__).resolve().parents[1]
    for candidate in (root / ".env", root / "tools" / ".env"):
        if not candidate.is_file():
            continue
        for raw in candidate.read_text(encoding="utf-8").splitlines():
            line = raw.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            key = key.strip()
            value = value.strip().strip('"').strip("'")
            if key and key not in os.environ:
                os.environ[key] = value


def main(argv: list[str] | None = None) -> int:
    load_dotenv()
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default=os.environ.get("LANGSTUDY_SYNC_HOST", "0.0.0.0"))
    parser.add_argument(
        "--port", type=int, default=int(os.environ.get("LANGSTUDY_SYNC_PORT", "8080"))
    )
    parser.add_argument(
        "--store",
        type=Path,
        default=Path(os.environ.get("LANGSTUDY_SYNC_STORE", "tools/var/sync-store")),
    )
    args = parser.parse_args(argv)

    server = create_server(args.host, args.port, args.store)
    host, port = server.server_address[:2]
    print(f"LangStudy sync server listening on http://{host}:{port}", file=sys.stderr)
    print(f"Snapshots: {args.store.resolve()}", file=sys.stderr)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
