#!/usr/bin/env python3
"""Host tests for the dependency-free LSPROG sync server."""

from __future__ import annotations

import http.client
import importlib.util
import sys
import tempfile
import threading
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "langstudy_sync_server", ROOT / "tools" / "langstudy_sync_server.py"
)
assert SPEC and SPEC.loader
SYNC = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = SYNC
SPEC.loader.exec_module(SYNC)


def make_progress(
    records: list[tuple[int, int, int, int, int, int]] | None = None,
    session_count: int = 0,
    answered_total: int = 0,
    correct_total: int = 0,
) -> SYNC.Progress:
    return SYNC.Progress(
        session_count=session_count,
        answered_total=answered_total,
        correct_total=correct_total,
        records=[
            SYNC.Record(
                word_id=word_id,
                box=box,
                correct=correct,
                wrong=wrong,
                revision=revision,
                due_session=due_session,
            )
            for word_id, box, correct, wrong, revision, due_session in (records or [])
        ],
    )


class RunningServer:
    def __init__(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.server = SYNC.create_server("127.0.0.1", 0, Path(self.directory.name))
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    def __enter__(self) -> "RunningServer":
        self.thread.start()
        return self

    def __exit__(self, *exc: object) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)
        self.directory.cleanup()

    def request(
        self, method: str, path: str, body: bytes | None = None, length: int | None = None
    ) -> tuple[int, bytes]:
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        try:
            headers = {"Connection": "close"}
            payload = body
            if length is not None:
                headers["Content-Length"] = str(length)
                payload = body if body is not None else b""
            if body is not None:
                headers.setdefault("Content-Type", "text/plain")
                headers.setdefault("Content-Length", str(len(body)))
            connection.request(method, path, body=payload, headers=headers)
            response = connection.getresponse()
            return response.status, response.read()
        finally:
            connection.close()


class ParseWriteTest(unittest.TestCase):
    def test_round_trip(self) -> None:
        original = make_progress(
            [(1001, 2, 3, 1, 4, 7), (1002, 0, 0, 2, 1, 2)],
            session_count=3,
            answered_total=6,
            correct_total=4,
        )
        parsed = SYNC.parse_progress(SYNC.write_progress(original))
        self.assertEqual(parsed, original)

    def test_rejects_bad_checksum(self) -> None:
        data = bytearray(SYNC.write_progress(make_progress([(1001, 1, 1, 0, 1, 3)])))
        data[-3] = ord("0") if data[-3] != ord("0") else ord("1")
        with self.assertRaisesRegex(SYNC.ProgressError, "checksum"):
            SYNC.parse_progress(bytes(data))

    def test_rejects_trailing_garbage(self) -> None:
        data = SYNC.write_progress(make_progress([(1001, 1, 1, 0, 1, 3)])) + b"x\n"
        with self.assertRaises(SYNC.ProgressError):
            SYNC.parse_progress(data)

    def test_rejects_duplicate_words(self) -> None:
        progress = make_progress(
            [(1001, 1, 1, 0, 1, 3), (1001, 1, 1, 0, 2, 3)]
        )
        with self.assertRaisesRegex(SYNC.ProgressError, "duplicate"):
            SYNC.write_progress(progress)

    def test_rejects_out_of_range_box(self) -> None:
        progress = make_progress([(1001, 5, 1, 0, 1, 3)])
        with self.assertRaisesRegex(SYNC.ProgressError, "box"):
            SYNC.write_progress(progress)


class MergeTest(unittest.TestCase):
    def test_higher_revision_wins(self) -> None:
        stored = make_progress([(1001, 1, 1, 0, 2, 3)])
        incoming = make_progress([(1001, 4, 9, 1, 3, 9)])
        merged = SYNC.merge_progress(stored, incoming)
        self.assertEqual(merged.records[0].revision, 3)
        self.assertEqual(merged.records[0].box, 4)

    def test_equal_revision_keeps_store(self) -> None:
        stored = make_progress([(1001, 1, 1, 0, 2, 3)])
        incoming = make_progress([(1001, 4, 9, 1, 2, 9)])
        merged = SYNC.merge_progress(stored, incoming)
        self.assertEqual(merged.records[0].box, 1)

    def test_appends_unknown_words(self) -> None:
        stored = make_progress([(1001, 1, 1, 0, 2, 3)])
        incoming = make_progress([(2001, 0, 0, 1, 1, 2)])
        merged = SYNC.merge_progress(stored, incoming)
        self.assertEqual([record.word_id for record in merged.records], [1001, 2001])

    def test_does_not_exceed_record_limit(self) -> None:
        stored = make_progress(
            [(1000 + offset, 0, 0, 0, 1, 2) for offset in range(SYNC.RECORDS_MAX)]
        )
        incoming = make_progress([(9001, 1, 1, 0, 1, 2)])
        merged = SYNC.merge_progress(stored, incoming)
        self.assertEqual(len(merged.records), SYNC.RECORDS_MAX)
        self.assertNotIn(9001, [record.word_id for record in merged.records])

    def test_more_sessions_adopt_counters(self) -> None:
        stored = make_progress(session_count=1, answered_total=2, correct_total=1)
        incoming = make_progress(session_count=5, answered_total=40, correct_total=33)
        merged = SYNC.merge_progress(stored, incoming)
        self.assertEqual(merged.session_count, 5)
        self.assertEqual(merged.answered_total, 40)
        self.assertEqual(merged.correct_total, 33)

    def test_fewer_sessions_keep_local_counters(self) -> None:
        stored = make_progress(session_count=5, answered_total=40, correct_total=39)
        incoming = make_progress(session_count=2, answered_total=8, correct_total=8)
        merged = SYNC.merge_progress(stored, incoming)
        self.assertEqual(merged.session_count, 5)
        self.assertEqual(merged.answered_total, 40)


class ServerTest(unittest.TestCase):
    def test_health_and_missing_snapshot(self) -> None:
        with RunningServer() as running:
            status, body = running.request("GET", "/health")
            self.assertEqual(status, 200)
            self.assertEqual(body, b"ok\n")
            status, _ = running.request("GET", "/devices/alice")
            self.assertEqual(status, 404)

    def test_post_then_get(self) -> None:
        progress = make_progress(
            [(1001, 2, 3, 1, 4, 7)], session_count=2, answered_total=4, correct_total=3
        )
        with RunningServer() as running:
            status, body = running.request(
                "POST", "/devices/alice", SYNC.write_progress(progress)
            )
            self.assertEqual(status, 200)
            self.assertEqual(SYNC.parse_progress(body), progress)

            status, stored = running.request("GET", "/devices/alice")
            self.assertEqual(status, 200)
            self.assertEqual(SYNC.parse_progress(stored), progress)

    def test_second_post_merges_and_is_idempotent(self) -> None:
        first = make_progress(
            [(1001, 1, 1, 0, 1, 3)], session_count=1, answered_total=1, correct_total=1
        )
        second = make_progress(
            [(1001, 2, 2, 0, 2, 4), (2001, 0, 0, 1, 1, 2)],
            session_count=2,
            answered_total=3,
            correct_total=2,
        )
        with RunningServer() as running:
            running.request("POST", "/devices/alice", SYNC.write_progress(first))
            status, body = running.request(
                "POST", "/devices/alice", SYNC.write_progress(second)
            )
            self.assertEqual(status, 200)
            merged = SYNC.parse_progress(body)
            self.assertEqual([record.word_id for record in merged.records], [1001, 2001])
            self.assertEqual(merged.session_count, 2)

            # Replaying the same snapshot changes nothing.
            status, replay = running.request(
                "POST", "/devices/alice", SYNC.write_progress(second)
            )
            self.assertEqual(status, 200)
            self.assertEqual(SYNC.parse_progress(replay), merged)

    def test_paths_are_isolated(self) -> None:
        alice = make_progress([(1001, 1, 1, 0, 1, 3)])
        bob = make_progress([(2001, 1, 1, 0, 1, 3)])
        with RunningServer() as running:
            running.request("POST", "/devices/alice", SYNC.write_progress(alice))
            running.request("POST", "/devices/bob", SYNC.write_progress(bob))
            self.assertEqual(
                SYNC.parse_progress(running.request("GET", "/devices/alice")[1]),
                alice,
            )
            self.assertEqual(
                SYNC.parse_progress(running.request("GET", "/devices/bob")[1]), bob
            )

    def test_rejects_checksum_failure(self) -> None:
        data = bytearray(SYNC.write_progress(make_progress([(1001, 1, 1, 0, 1, 3)])))
        data[-2] = ord("0") if data[-2] != ord("0") else ord("1")
        with RunningServer() as running:
            status, body = running.request("POST", "/devices/alice", bytes(data))
            self.assertEqual(status, 400)
            self.assertIn(b"checksum", body)

    def test_rejects_oversized_body(self) -> None:
        with RunningServer() as running:
            status, _ = running.request(
                "POST", "/devices/alice", b"x", length=SYNC.PROGRESS_TEXT_MAX + 1
            )
            self.assertEqual(status, 413)

    def test_rejects_missing_content_length(self) -> None:
        with RunningServer() as running:
            connection = http.client.HTTPConnection(
                "127.0.0.1", running.port, timeout=10
            )
            try:
                connection.putrequest("POST", "/devices/alice")
                connection.putheader("Connection", "close")
                connection.endheaders()
                response = connection.getresponse()
                self.assertEqual(response.status, 411)
            finally:
                connection.close()


class StoreKeyTest(unittest.TestCase):
    def test_default_path(self) -> None:
        self.assertEqual(SYNC.store_key("/"), "default.lsprog")

    def test_nested_path_is_flattened(self) -> None:
        self.assertEqual(
            SYNC.store_key("/devices/alice/slot-1"),
            "devices__alice__slot-1.lsprog",
        )

    def test_query_is_ignored(self) -> None:
        self.assertEqual(SYNC.store_key("/alice?token=1"), "alice.lsprog")

    def test_rejects_traversal(self) -> None:
        for path in ("/../etc/passwd", "/a/../../b", "/a\\b", "/a%2Fb"):
            with self.assertRaises(SYNC.ProgressError):
                SYNC.store_key(path)


class DotenvTest(unittest.TestCase):
    def test_loads_without_overriding_environment(self) -> None:
        import os

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "tools").mkdir()
            (root / ".env").write_text(
                "# comment\nLANGSTUDY_SYNC_PORT=9090\nLANGSTUDY_SYNC_HOST='127.0.0.1'\n",
                encoding="utf-8",
            )
            (root / "tools" / ".env").write_text(
                "LANGSTUDY_SYNC_STORE=tools/var/test\nLANGSTUDY_SYNC_PORT=1\n",
                encoding="utf-8",
            )
            keys = ("LANGSTUDY_SYNC_PORT", "LANGSTUDY_SYNC_HOST", "LANGSTUDY_SYNC_STORE")
            saved = {key: os.environ.get(key) for key in keys}
            try:
                for key in keys:
                    os.environ.pop(key, None)
                os.environ["LANGSTUDY_SYNC_PORT"] = "7777"
                SYNC.load_dotenv(root)
                self.assertEqual(os.environ["LANGSTUDY_SYNC_PORT"], "7777")
                self.assertEqual(os.environ["LANGSTUDY_SYNC_HOST"], "127.0.0.1")
                self.assertEqual(os.environ["LANGSTUDY_SYNC_STORE"], "tools/var/test")
            finally:
                for key, value in saved.items():
                    if value is None:
                        os.environ.pop(key, None)
                    else:
                        os.environ[key] = value


if __name__ == "__main__":
    unittest.main()
