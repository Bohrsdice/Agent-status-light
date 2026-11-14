"""Read-only, single-conversation Codex lifecycle monitor (Windows / stdlib)."""
from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time

BASE = Path(__file__).resolve().parent


def timestamp(value):
    try:
        return datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp()
    except (AttributeError, ValueError, TypeError):
        return None


@dataclass
class TaskState:
    state: str = "unknown"
    turn_id: str | None = None
    event: str | None = None
    event_at: str | None = None
    started_at: str | None = None
    reason: str = "尚未找到任务事件"
    pending_count: int = 0

    @property
    def value(self):
        return {"running": 1, "completed": 0}.get(self.state)


class Lifecycle:
    """Only explicit lifecycle events can change the binary state."""

    def __init__(self, thread_id):
        self.thread_id = thread_id
        self.identity_ok = False
        self.state = TaskState()
        self.pending_questions = set()
        self.answered_questions = set()
        self.turn_finished = False

    def consume_question_item(self, item, when):
        if not isinstance(item, dict):
            return
        kind = str(item.get("type", "")).lower()
        if kind == "agentmessage" and item.get("delivery") == "async":
            questions, call_id = item.get("questions"), item.get("id")
            if not isinstance(questions, list) or not isinstance(call_id, str) or not call_id:
                return
            self.pending_questions.update(
                (call_id, index) for index, question in enumerate(questions)
                if isinstance(question, dict) and (call_id, index) not in self.answered_questions
            )
        elif kind == "usermessage":
            content = item.get("content", [])
            if not isinstance(content, list):
                return
            # Parse only the native reply envelope and its exact question IDs.
            # Normal prose, tool output, and question marks never control the lamp.
            for part in content:
                if not isinstance(part, dict) or not isinstance(part.get("text"), str):
                    continue
                text = part["text"].strip()
                opening, closing = "<send_user_message_question_reply>", "</send_user_message_question_reply>"
                if not (text.startswith(opening) and text.endswith(closing)):
                    continue
                try:
                    replies = json.loads(text[len(opening):-len(closing)])
                except ValueError:
                    continue
                if not isinstance(replies, list):
                    continue
                for reply in replies:
                    if not isinstance(reply, dict):
                        continue
                    try:
                        key = json.loads(reply.get("questionItemId", ""))
                    except (ValueError, TypeError):
                        continue
                    if (isinstance(key, list) and len(key) == 3
                            and key[0] == "request_user_input_async"
                            and isinstance(key[1], str) and type(key[2]) is int):
                        question_id = (key[1], key[2])
                        if question_id in self.pending_questions:
                            self.pending_questions.remove(question_id)
                            self.answered_questions.add(question_id)
        else:
            return
        if self.pending_questions:
            self.state = TaskState("waiting", self.state.turn_id, "question_pending", when,
                                   self.state.started_at, "等待你回答或确认", len(self.pending_questions))
        elif self.state.state == "waiting":
            self.state = TaskState("completed" if self.turn_finished else "running",
                                   self.state.turn_id, "question_answered", when,
                                   self.state.started_at,
                                   "确认已收到；本轮已结束" if self.turn_finished else "确认已收到；继续运行")

    def consume(self, record):
        if not isinstance(record, dict):
            return
        payload = record.get("payload")
        if not isinstance(payload, dict):
            return
        if record.get("type") == "session_meta":
            self.identity_ok = payload.get("id") == self.thread_id
            if not self.identity_ok:
                self.state = TaskState(reason="会话标识不匹配")
            return
        if not self.identity_ok or record.get("type") != "event_msg":
            return
        if payload.get("thread_id", self.thread_id) != self.thread_id:
            return
        kind = payload.get("type")
        turn_id = payload.get("turn_id")
        if not turn_id or not isinstance(turn_id, str):
            return
        when = record.get("timestamp")
        if kind == "task_started":
            self.pending_questions.clear()
            self.answered_questions.clear()
            self.turn_finished = False
            self.state = TaskState("running", turn_id, kind, when, when,
                                   "已收到开始事件；本轮尚未结束")
        elif kind == "item_completed":
            if turn_id == self.state.turn_id and self.state.state in ("running", "waiting", "completed"):
                self.consume_question_item(payload.get("item"), when)
        elif kind in ("task_complete", "turn_aborted"):
            # Never let a delayed completion from an older turn finish a newer one.
            if turn_id != self.state.turn_id or self.state.state not in ("running", "waiting"):
                return
            if kind == "task_complete":
                self.turn_finished = True
                if self.pending_questions:
                    self.state = TaskState("waiting", turn_id, kind, when, self.state.started_at,
                                           "本轮已结束，仍等待你回答或确认", len(self.pending_questions))
                else:
                    self.state = TaskState("completed", turn_id, kind, when,
                                           self.state.started_at, "已收到本轮结束事件")
            else:
                self.pending_questions.clear()
                self.state = TaskState("interrupted", turn_id, kind, when,
                                       self.state.started_at, "本轮已中断，未标记为完成")


class RolloutReader:
    """Incremental binary reads preserve partial JSON lines and split UTF-8."""

    def __init__(self, path, thread_id):
        self.path = Path(path)
        self.thread_id = thread_id
        self.lifecycle = Lifecycle(thread_id)
        self.offset = 0
        self.pending = b""
        self.identity = None
        self.error = None
        self.damaged = False

    def poll(self):
        try:
            with self.path.open("rb") as stream:
                stat = os.fstat(stream.fileno())
                identity = (stat.st_dev, stat.st_ino)
                if identity != self.identity or stat.st_size < self.offset:
                    self.lifecycle = Lifecycle(self.thread_id)
                    self.offset = 0
                    self.pending = b""
                    self.damaged = False
                    self.identity = identity
                stream.seek(self.offset)
                # Bound each GUI tick's work for very large historical sessions.
                chunk = stream.read(4 * 1024 * 1024)
                self.offset = stream.tell()
                catching_up = self.offset < stat.st_size
            lines = (self.pending + chunk).split(b"\n")
            self.pending = lines.pop()
            for line in lines:
                if not line.strip():
                    continue
                try:
                    record = json.loads(line)
                except (ValueError, UnicodeDecodeError):
                    self.damaged = True
                    continue
                self.lifecycle.consume(record)
                if (isinstance(record, dict) and record.get("type") == "event_msg"
                        and isinstance(record.get("payload"), dict)
                        and record["payload"].get("type") == "task_started"
                        and self.lifecycle.identity_ok
                        and record["payload"].get("thread_id", self.thread_id) == self.thread_id
                        and self.lifecycle.state.event == "task_started"
                        and self.lifecycle.state.turn_id == record["payload"].get("turn_id")):
                    # A new explicit start provides a fresh synchronization point.
                    self.damaged = False
            self.error = "正在读取历史事件" if catching_up else (
                "事件记录损坏，等待下一轮开始事件" if self.damaged else None)
        except OSError:
            self.error = "会话记录暂时不可读"
        return self.lifecycle.state


@dataclass(frozen=True)
class RuntimeInfo:
    connected: bool = False
    started: float | None = None
    reason: str = "正在检查 Codex 后台服务"


def discover_runtime():
    """Read only process metadata; do not attach to/restart Codex's app server."""
    command = (
        "$items = @(Get-CimInstance Win32_Process -Filter \"Name = 'codex.exe'\" "
        "| Where-Object { $_.CommandLine -match '\\bapp-server\\b' }); "
        "ConvertTo-Json -Compress -InputObject @($items | ForEach-Object { "
        "@{ pid=$_.ProcessId; started=$_.CreationDate.ToUniversalTime().ToString('o') } })"
    )
    try:
        result = subprocess.run(
            ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", command],
            capture_output=True, timeout=10, creationflags=0x08000000,
        )
        if result.returncode:
            return RuntimeInfo(reason="无法确认 Codex 后台服务状态")
        processes = json.loads(result.stdout.decode("utf-8-sig"))
        if len(processes) != 1:
            return RuntimeInfo(reason="Codex 后台服务未运行" if not processes else
                               "发现多个 Codex 后台服务，暂不判断")
        started = timestamp(processes[0]["started"])
        if started is None:
            return RuntimeInfo(reason="无法确认 Codex 后台服务启动时间")
        return RuntimeInfo(True, started, "Codex 后台服务在线")
    except (OSError, ValueError, KeyError, TypeError, subprocess.TimeoutExpired):
        return RuntimeInfo(reason="无法确认 Codex 后台服务状态")


class RuntimeProbe:
    def __init__(self):
        self.info = RuntimeInfo()
        self.stopping = threading.Event()
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        while not self.stopping.is_set():
            self.info = discover_runtime()
            self.stopping.wait(5)


def snapshot(reader, runtime):
    state = reader.poll()
    result = {**asdict(state), "value": state.value,
              "thread_id": reader.thread_id, "source": str(reader.path),
              "runtime_connected": runtime.connected}
    reason = reader.error
    if not reason and not runtime.connected:
        reason = runtime.reason
    if not reason and state.state in ("running", "waiting"):
        started = timestamp(state.started_at)
        if started is None or runtime.started is None or started < runtime.started:
            reason = "未结束记录早于当前后台服务，等待新的任务事件"
    if reason:
        result.update(state="unknown", value=None, reason=reason)
    return result


class Publisher:
    def __init__(self, directory):
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        self.previous = None
        self.last_write = 0.0

    def publish(self, data):
        changed = data != self.previous
        now = time.monotonic()
        # Heartbeat lets another program detect that this monitor has stopped.
        if not changed and now - self.last_write < 2:
            return
        output = {**data, "observed_at": datetime.now(timezone.utc).isoformat(),
                  "monitor_pid": os.getpid()}
        destination = self.directory / "status.json"
        temporary = self.directory / f"status.{os.getpid()}.tmp"
        temporary.write_text(json.dumps(output, ensure_ascii=False, indent=2), encoding="utf-8")
        temporary.replace(destination)
        if changed:
            with (self.directory / "transitions.jsonl").open("a", encoding="utf-8") as stream:
                stream.write(json.dumps(output, ensure_ascii=False) + "\n")
        self.previous = data.copy()
        self.last_write = now


def locate_rollout(home, thread_id):
    for directory in (home / "sessions", home / "archived_sessions"):
        matches = list(directory.glob(f"**/*{thread_id}.jsonl"))
        if len(matches) == 1:
            return matches[0]
        if len(matches) > 1:
            raise ValueError("找到多个同名会话记录，请通过 --rollout 指定文件")
    raise FileNotFoundError("未找到指定会话的本地记录")


def load_options(args):
    config_path = Path(args.config)
    config = json.loads(config_path.read_text(encoding="utf-8-sig")) if config_path.exists() else {}
    thread_id = args.thread or config.get("thread_id") or os.environ.get("CODEX_THREAD_ID")
    if not thread_id:
        raise ValueError("请在 config.json 中填写 thread_id，或使用 --thread")
    import uuid
    uuid.UUID(thread_id)  # Avoid accidentally treating an ID as a glob/path expression.
    home = Path(config.get("codex_home") or os.environ.get("CODEX_HOME") or Path.home() / ".codex")
    rollout = Path(args.rollout) if args.rollout else locate_rollout(home, thread_id)
    return config, RolloutReader(rollout, thread_id)


def main():
    if sys.stdout is not None:
        sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", default=str(BASE / "config.json"))
    parser.add_argument("--thread")
    parser.add_argument("--rollout", help="Exact local rollout JSONL (read only)")
    parser.add_argument("--output-dir", default=str(BASE / "runtime"))
    parser.add_argument("--once", action="store_true", help="Print one state, no window")
    parser.add_argument("--watch", action="store_true", help="Headless JSON state stream")
    parser.add_argument("--test-yellow", action="store_true", help="Preview yellow for 8 seconds in the running tray")
    args = parser.parse_args()
    if args.test_yellow:
        output = Path(args.output_dir)
        current = json.loads((output / "status.json").read_text(encoding="utf-8"))
        observed = timestamp(current.get("observed_at"))
        if observed is None or time.time() - observed > 15:
            raise RuntimeError("请先启动红绿灯监控器")
        temporary = output / f"display-request.{os.getpid()}.tmp"
        temporary.write_text(json.dumps({"action": "test-yellow", "expires_at": time.time() + 8}), encoding="utf-8")
        temporary.replace(output / "display-request.json")
        print("已请求 8 秒黄灯演示；真实任务状态不变")
        return
    config, reader = load_options(args)
    if args.once:
        runtime = discover_runtime()
        data = snapshot(reader, runtime)
        while reader.error == "正在读取历史事件":
            data = snapshot(reader, runtime)
        print(json.dumps(data, ensure_ascii=False, indent=2))
        return
    # One writer per output directory, so two launches cannot race on status.json.
    import ctypes
    import hashlib
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateMutexW.argtypes = [ctypes.c_void_p, ctypes.c_bool, ctypes.c_wchar_p]
    kernel.CreateMutexW.restype = ctypes.c_void_p
    key = hashlib.sha256(str(Path(args.output_dir).resolve()).casefold().encode()).hexdigest()[:24]
    mutex = kernel.CreateMutexW(None, False, "Local\\CodexLight_" + key)
    if not mutex:
        raise OSError("无法创建监控器实例锁")
    if ctypes.get_last_error() == 183:
        if args.watch:
            print("该状态目录已有监控器运行", file=sys.stderr)
        return
    publisher = Publisher(args.output_dir)
    if args.watch:
        probe = RuntimeProbe()
        previous = None
        try:
            while True:
                data = snapshot(reader, probe.info)
                publisher.publish(data)
                if data != previous:
                    print(json.dumps(data, ensure_ascii=False), flush=True)
                    previous = data
                time.sleep(0.5)
        except KeyboardInterrupt:
            probe.stopping.set()
            data.update(state="unknown", value=None, reason="监控器已关闭")
            publisher.publish(data)
    else:
        from tray_win32 import run_tray
        run_tray(reader, config, publisher)


if __name__ == "__main__":
    try:
        main()
    except Exception:
        import traceback
        error = traceback.format_exc()
        (BASE / "startup-error.log").write_text(error, encoding="utf-8")
        if sys.stderr is not None:
            print(error, file=sys.stderr)
        else:
            import ctypes
            ctypes.windll.user32.MessageBoxW(None, "启动失败，请查看 startup-error.log", "Codex 红绿灯", 16)
        sys.exit(1)
