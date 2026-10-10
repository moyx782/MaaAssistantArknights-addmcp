"""Local stdio MCP adapter. No simulated game state and no raw input tools."""

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import sys
import threading
import time
from collections import deque
from typing import Literal

from mcp.server.fastmcp import FastMCP
from pydantic import BaseModel, ConfigDict, Field

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "src" / "Python"))


class Preconditions(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)
    connected: bool = True
    running: bool


class Action(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)
    kind: Literal["deploy", "retreat", "skill", "speed", "pause"]
    expected_state_hash: str = Field(pattern=r"^[0-9a-f]{64}$")
    idempotency_key: str = Field(min_length=1, max_length=128)
    timeout_ms: int = Field(ge=1, le=30000)
    safety_gate: bool
    preconditions: Preconditions
    payload: dict = Field(default_factory=dict)


class CoreBackend:
    """Owns one real Asst instance, never attaches to a separate GUI process."""

    def __init__(self, core_dir, adb=None, address=None, config="General"):
        from asst.asst import Asst

        self.lock = threading.RLock()
        self.events = deque(maxlen=100)
        self.revision = 0
        self.callback_errors = 0
        self.core = None
        # Retain the C function pointer for the whole native instance lifetime.
        self.callback = Asst.CallBackType(self.on_callback)
        if not Asst.load(Path(core_dir).resolve()):
            raise RuntimeError("MaaCore resource initialization failed")
        self.core = Asst(self.callback)
        if not self.core._Asst__ptr:
            raise RuntimeError("AsstCreateEx returned a null handle")
        # The repository wrapper lacks these two read-only methods. Bind the
        # existing exported C API, rather than adding an OCR or input path.
        self.lib = Asst._Asst__lib
        self.lib.AsstConnected.argtypes = [ctypes.c_void_p]
        self.lib.AsstConnected.restype = ctypes.c_bool
        self.lib.AsstGetTasksList.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int32), ctypes.c_uint64]
        self.lib.AsstGetTasksList.restype = ctypes.c_uint64
        if adb and address and not self.core.connect(adb, address, config):
            raise RuntimeError("AsstConnect failed")

    def on_callback(self, message, raw, _arg):
        # Never throw across a ctypes callback boundary. Events are evidence of
        # what MaaCore reported, not a complete/current game-state snapshot.
        try:
            details = json.loads(raw.decode("utf-8")) if raw else {}
            with self.lock:
                self.revision += 1
                self.events.append({"sequence": self.revision, "message": message,
                                    "received_at": time.time(), "details": details})
        except (ValueError, UnicodeError):
            with self.lock:
                self.callback_errors += 1
                self.revision += 1

    def snapshot(self):
        with self.lock:
            handle = self.core._Asst__ptr
            task_ids = (ctypes.c_int32 * 1024)()
            count = int(self.lib.AsstGetTasksList(handle, task_ids, len(task_ids)))
            if count > len(task_ids):
                raise RuntimeError("AsstGetTasksList overflow or error")
            return {"source": "MaaCore C API / AsstCreateEx callbacks", "scope": "owned_instance",
                    "connected": bool(self.lib.AsstConnected(handle)),
                    "running": bool(self.core.running()), "core_version": self.core.get_version(),
                    "task_ids": list(task_ids[:count]), "revision": self.revision,
                    "events": list(self.events), "callback_errors": self.callback_errors,
                    "battle_state": None, "battle_state_status": "unsupported"}

    def run_operator_task(self, task_file):
        """Only an explicit local CLI file starts a task, never a model tool."""
        task = json.loads(Path(task_file).read_text(encoding="utf-8"))
        if not isinstance(task, dict) or set(task) != {"type", "params"}:
            raise ValueError("task file must contain exactly type and params")
        if task["type"] not in {"Fight", "Roguelike"} or not isinstance(task["params"], dict):
            raise ValueError("only explicit Fight/Roguelike task files are accepted")
        if not self.lib.AsstConnected(self.core._Asst__ptr):
            raise RuntimeError("connect a device before starting a task")
        if self.core.append_task(task["type"], task["params"]) <= 0 or not self.core.start():
            raise RuntimeError("MaaCore rejected task append/start")

    def close(self):
        if self.core is not None and self.core._Asst__ptr:
            # Let the wrapper own destruction exactly once.  Stop first so a
            # running task is not left behind; this instance is never another
            # GUI process, so it is safe to stop its queue.
            try:
                self.core.stop()
            finally:
                core = self.core
                self.core = None
                del core


class DecisionService:
    def __init__(self, backend=None):
        self.backend = backend
        self.lock = threading.Lock()
        self.results = {}

    def observe(self):
        if self.backend is None:
            return {"status": "not_configured", "state": None,
                    "reason": "Pass --core-dir for a real MaaCore instance; no synthetic state is used."}
        state = self.backend.snapshot()
        digest = hashlib.sha256(json.dumps(state, sort_keys=True, separators=(",", ":"),
                                          ensure_ascii=False).encode()).hexdigest()
        return {"status": "ok", "state_hash": digest, "state": state,
                "observed_at": time.time(), "supported_actions": []}

    def roguelike(self):
        result = self.observe()
        if result["status"] != "ok":
            return result
        # Preserve original callback fields rather than inferring floor, HP,
        # inventory or map nodes that the callback did not actually provide.
        events = [e for e in result["state"]["events"]
                  if isinstance(e["details"], dict) and e["details"].get("taskchain") == "Roguelike"]
        return {"status": "partial" if events else "unavailable", "state_hash": result["state_hash"],
                "events": events, "complete_state": None, "read_only": True,
                "reason": "Bounded historical callbacks only; complete current roguelike state is unavailable."}

    def action(self, request):
        # There is deliberately no executor for these actions: the public C API
        # has no deploy/retreat/skill/speed/pause primitive. Never map pause to
        # AsstStop (which stops and clears the task queue).
        started = time.monotonic()
        with self.lock:
            fingerprint = request.model_dump_json()
            previous = self.results.get(request.idempotency_key)
            if previous:
                if previous[0] != fingerprint:
                    return {"status": "idempotency_conflict", "executed": False}
                return {**previous[1], "replayed": True}
            if not request.safety_gate:
                return {"status": "safety_denied", "executed": False}
            observed = self.observe()
            if observed["status"] != "ok":
                return {"status": observed["status"], "executed": False}
            if (time.monotonic() - started) * 1000 >= request.timeout_ms:
                return {"status": "timeout", "executed": False}
            if observed["state_hash"] != request.expected_state_hash:
                return {"status": "conflict", "executed": False}
            state = observed["state"]
            if any(state[name] != value for name, value in request.preconditions.model_dump().items()):
                return {"status": "precondition_failed", "executed": False}
            result = {"status": "unsupported", "executed": False,
                      "reason": "MaaCore public C API does not expose atomic battle actions."}
            # Bound cache memory; do not evict an old key and risk re-execution.
            if len(self.results) >= 4096:
                return {"status": "capacity_exceeded", "executed": False}
            self.results[request.idempotency_key] = (fingerprint, result)
            return result


def create_server(service):
    server = FastMCP("maa-decision-layer")

    @server.tool()
    def observe_state() -> dict:
        """Read this server's real MaaCore task status and callback history. No full battle state."""
        return service.observe()

    @server.tool()
    def observe_roguelike_state() -> dict:
        """Read only actual Roguelike callbacks; unavailable fields remain null."""
        return service.roguelike()

    @server.tool()
    def observe_tasks() -> dict:
        """Read native task IDs and running flag for this owned instance, not another GUI."""
        result = service.observe()
        if result["status"] != "ok":
            return result
        return {"status": "ok", "task_ids": result["state"]["task_ids"],
                "running": result["state"]["running"], "scope": "owned_instance"}

    @server.tool()
    def battle_action(request: Action) -> dict:
        """Validate CAS/safety/preconditions; all five battle actions currently return unsupported."""
        return service.action(request)

    return server


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core-dir", help="Directory containing real MaaCore library and resource/")
    parser.add_argument("--adb")
    parser.add_argument("--address")
    parser.add_argument("--config", default="General")
    parser.add_argument("--task-file", help="Explicit local Fight/Roguelike task JSON; starts native automation")
    args = parser.parse_args()
    if bool(args.adb) != bool(args.address) or ((args.adb or args.task_file) and not args.core_dir):
        parser.error("--adb and --address must be paired and require --core-dir; tasks require --core-dir")
    # Preserve an unpolluted JSON-RPC stdout even if the native library prints.
    protocol_fd = os.dup(sys.stdout.fileno())
    os.dup2(sys.stderr.fileno(), sys.stdout.fileno())
    sys.stdout = os.fdopen(protocol_fd, "w", encoding="utf-8", buffering=1)
    backend = None
    try:
        if args.core_dir:
            backend = CoreBackend(args.core_dir, args.adb, args.address, args.config)
            if args.task_file:
                backend.run_operator_task(args.task_file)
        create_server(DecisionService(backend)).run(transport="stdio")
    finally:
        if backend is not None:
            backend.close()


if __name__ == "__main__":
    main()
