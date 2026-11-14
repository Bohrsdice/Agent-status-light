import json
from pathlib import Path
import tempfile
import unittest

from codex_light import Lifecycle, Publisher, RolloutReader, RuntimeInfo, snapshot

THREAD = "test-thread"
RUNTIME = RuntimeInfo(True, 0, "online")


def meta(thread=THREAD):
    return {"type": "session_meta", "payload": {"id": thread}}


def event(kind, turn="a", **extra):
    return {"type": "event_msg", "timestamp": "2026-11-13T08:00:00Z",
            "payload": {"type": kind, "turn_id": turn, **extra}}


def line(record):
    return (json.dumps(record, ensure_ascii=False) + "\n").encode("utf-8")


class LifecycleTests(unittest.TestCase):
    def setUp(self):
        self.machine = Lifecycle(THREAD)
        self.machine.consume(meta())

    def test_explicit_start_and_matching_end(self):
        self.machine.consume(event("task_started"))
        self.assertEqual(self.machine.state.value, 1)
        self.machine.consume(event("task_complete"))
        self.assertEqual(self.machine.state.value, 0)

    def test_stale_completion_cannot_finish_new_turn(self):
        self.machine.consume(event("task_started", "a"))
        self.machine.consume(event("task_started", "b"))
        self.machine.consume(event("task_complete", "a"))
        self.assertEqual(self.machine.state.value, 1)
        self.assertEqual(self.machine.state.turn_id, "b")

    def test_wrong_thread_is_ignored(self):
        self.machine.consume(event("task_started", thread_id="other"))
        self.assertIsNone(self.machine.state.value)
        self.machine.consume(meta("other"))
        self.machine.consume(event("task_started"))
        self.assertIsNone(self.machine.state.value)

    def test_orphan_completion_never_turns_green(self):
        self.machine.consume(event("task_complete"))
        self.assertIsNone(self.machine.state.value)

    def test_interrupted_is_not_complete_even_with_late_completion(self):
        self.machine.consume(event("task_started"))
        self.machine.consume(event("turn_aborted"))
        self.machine.consume(event("task_complete"))
        self.assertEqual(self.machine.state.state, "interrupted")
        self.assertIsNone(self.machine.state.value)

    def test_message_or_tool_completion_is_not_task_completion(self):
        self.machine.consume(event("task_started"))
        for kind in ("agent_message", "item_completed", "token_count", "error"):
            self.machine.consume(event(kind))
        self.assertEqual(self.machine.state.value, 1)


class ReaderTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "session.jsonl"
        self.path.write_bytes(line(meta()))
        self.reader = RolloutReader(self.path, THREAD)

    def append(self, data):
        with self.path.open("ab") as stream:
            stream.write(data)

    def state(self, runtime=RUNTIME):
        return snapshot(self.reader, runtime)

    def test_partial_utf8_and_partial_end_are_not_prematurely_parsed(self):
        data = line(event("task_started", note="中文"))
        cut = data.index("中".encode()) + 1
        self.append(data[:cut])
        self.assertIsNone(self.state()["value"])
        self.append(data[cut:])
        self.assertEqual(self.state()["value"], 1)
        end = line(event("task_complete"))
        self.append(end[:-1])
        self.assertEqual(self.state()["value"], 1)
        self.append(end[-1:])
        self.assertEqual(self.state()["value"], 0)

    def test_missing_file_clears_green_and_recovers(self):
        self.append(line(event("task_started")) + line(event("task_complete")))
        self.assertEqual(self.state()["value"], 0)
        self.path.unlink()
        self.assertIsNone(self.state()["value"])
        self.path.write_bytes(line(meta()) + line(event("task_started", "b")))
        self.assertEqual(self.state()["value"], 1)

    def test_truncation_resets_old_state(self):
        self.append(line(event("task_started")) + line(event("task_complete")))
        self.assertEqual(self.state()["value"], 0)
        self.path.write_bytes(line(meta()))
        self.assertIsNone(self.state()["value"])

    def test_corruption_does_not_keep_old_green(self):
        self.append(line(event("task_started")) + line(event("task_complete")))
        self.assertEqual(self.state()["value"], 0)
        self.append(b"broken record\n")
        self.assertIsNone(self.state()["value"])
        self.append(line(event("task_started", "b")))
        self.assertEqual(self.state()["value"], 1)

    def test_runtime_shutdown_or_restart_clears_running(self):
        self.append(line(event("task_started")))
        self.assertEqual(self.state()["value"], 1)
        self.assertIsNone(self.state(RuntimeInfo())["value"])
        self.assertIsNone(self.state(RuntimeInfo(True, 9999999999))["value"])

    def test_unrelated_start_does_not_recover_corruption(self):
        self.append(line(event("task_started")) + b"broken\n")
        self.assertIsNone(self.state()["value"])
        self.append(line(event("task_started", "b", thread_id="other")))
        self.assertIsNone(self.state()["value"])

    def test_replaced_file_replays_even_when_larger(self):
        self.append(line(event("task_started")) + line(event("task_complete")))
        self.assertEqual(self.state()["value"], 0)
        replacement = self.path.with_suffix(".new")
        replacement.write_bytes(line(meta()) + line(event("task_started", "b", padding="x" * 1000)))
        replacement.replace(self.path)
        self.assertEqual(self.state()["value"], 1)
        self.assertEqual(self.state()["turn_id"], "b")

    def test_disconnected_runtime_does_not_report_green(self):
        self.append(line(event("task_started")) + line(event("task_complete")))
        self.assertIsNone(self.state(RuntimeInfo())["value"])

    def test_publisher_metadata_and_transition_deduplication(self):
        publisher = Publisher(Path(self.temp.name) / "output")
        self.append(line(event("task_started")))
        data = self.state()
        publisher.publish(data)
        publisher.publish(data)
        output = publisher.directory
        self.assertEqual(len((output / "transitions.jsonl").read_text(encoding="utf-8").splitlines()), 1)
        published = json.loads((output / "status.json").read_text(encoding="utf-8"))
        self.assertEqual(published["value"], 1)
        self.assertIn("observed_at", published)
        self.append(line(event("task_complete")))
        publisher.publish(self.state())
        self.assertEqual(json.loads((output / "status.json").read_text(encoding="utf-8"))["value"], 0)


if __name__ == "__main__":
    unittest.main()
