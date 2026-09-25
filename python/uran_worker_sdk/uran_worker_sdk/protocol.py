"""Small, dependency-free JSONL worker server."""

import json
import queue
import sys
import threading
import time
from dataclasses import dataclass, field
from typing import Any, BinaryIO, Dict, Iterable, Mapping, Optional, Tuple

PROTOCOL = "uran.worker.jsonl.v1"
TERMINAL_STATES = frozenset(("succeeded", "failed", "cancelled", "rejected"))
MAX_FRAME_BYTES = 1024 * 1024


@dataclass(frozen=True)
class Error:
    code: str
    message: str
    retryable: bool = False
    details: Mapping[str, Any] = field(default_factory=dict)

    def as_dict(self) -> Dict[str, Any]:
        return {
            "code": self.code,
            "message": self.message,
            "retryable": self.retryable,
            "details": dict(self.details),
        }


class Worker:
    """Override lifecycle methods in a plugin worker."""

    plugin_id = "unknown"
    plugin_version = "0.0.0"
    capabilities: Iterable[str] = ()

    def __init__(self) -> None:
        self.identity: Mapping[str, str] = {}
        self.host: Mapping[str, Any] = {}
        self._server = None

    def hello(self, identity: Mapping[str, str], host: Mapping[str, Any]) -> None:
        """Inspect the trusted Runtime identity and negotiated host limits."""
        del identity, host

    def configure(self, config: Mapping[str, Any]) -> None:
        del config

    def activate(self) -> None:
        pass

    def deactivate(self) -> None:
        pass

    def invoke(self, request: Mapping[str, Any]) -> Mapping[str, Any]:
        raise NotImplementedError

    def cancel(self, operation_id: str) -> Mapping[str, Any]:
        del operation_id
        raise NotImplementedError("cancel is not implemented")

    def subscribe(self, subscription_id: str, field_prefix: str) -> None:
        del subscription_id, field_prefix

    def on_state(self, subscription_id: str, field: str,
                 value: Mapping[str, Any], sequence: int) -> None:
        """Receive a state event after a successful subscription."""
        del subscription_id, field, value, sequence

    def on_operation(self, result: Mapping[str, Any], source: Mapping[str, str]) -> None:
        """Receive a provider result for an asynchronous local dispatch."""
        del result, source

    def shutdown(self) -> None:
        pass

    def connect(self) -> None:
        raise NotImplementedError("protocol connect is not implemented")

    def disconnect(self) -> None:
        raise NotImplementedError("protocol disconnect is not implemented")

    def publish(self, message: Mapping[str, Any]) -> Mapping[str, Any]:
        del message
        raise NotImplementedError("protocol publish is not implemented")

    def emit_operation(self, operation_id: str, sequence: int, status: str,
                       payload: Optional[Mapping[str, Any]] = None,
                       message_type: str = "uran.operations.state.v1") -> None:
        if not self._server:
            raise RuntimeError("worker is not bound to a server")
        self._server.emit_operation(operation_id, sequence, status, payload, message_type)

    def emit_downlink(self, message_type: str, payload: Mapping[str, Any],
                      correlation_id: str = "",
                      target: Optional[Mapping[str, str]] = None) -> None:
        if not self._server:
            raise RuntimeError("worker is not bound to a server")
        self._server.emit_downlink(message_type, payload, correlation_id, target)

    def dispatch_local(self, message: Mapping[str, Any]) -> Mapping[str, Any]:
        if not self._server:
            raise RuntimeError("worker is not bound to a server")
        return self._server.host_request("dispatch", message=dict(message))

    def publish_uplink(self, message: Mapping[str, Any]) -> Mapping[str, Any]:
        if not self._server:
            raise RuntimeError("worker is not bound to a server")
        return self._server.host_request("publish", message=dict(message))

    def call_local(self, message: Mapping[str, Any], timeout: float = 5.0) -> Mapping[str, Any]:
        """Wait for a targeted provider's terminal result, not its queue ACK."""
        if not self._server:
            raise RuntimeError("worker is not bound to a server")
        return self._server.call_local(message, timeout)

    def state_snapshot(self) -> Mapping[str, Any]:
        if not self._server:
            raise RuntimeError("worker is not bound to a server")
        return self._server.host_request("state.snapshot")["result"]

    def runtime_snapshot(self) -> Mapping[str, Any]:
        """Return the canonical Runtime snapshot, including components."""
        if not self._server:
            raise RuntimeError("worker is not bound to a server")
        return self._server.host_request("runtime.snapshot")["result"]

    def set_state(self, field: str, value: Any, persistent: bool = False) -> Mapping[str, Any]:
        if not self._server:
            raise RuntimeError("worker is not bound to a server")
        return self._server.host_request("state.set", field=field, value=value,
                                         persistent=persistent)

    def register_state(self, field: str, value_type: str = "any", unit: str = "",
                       persistent: bool = False) -> Mapping[str, Any]:
        if not self._server:
            raise RuntimeError("worker is not bound to a server")
        return self._server.host_request("state.register", field=field,
                                         type=value_type, unit=unit,
                                         persistent=persistent)


class WorkerServer:
    """Runs one worker over stdin/stdout without importing framework or ROS."""

    def __init__(self, worker: Worker, input_stream: Optional[BinaryIO] = None,
                 output_stream: Optional[BinaryIO] = None,
                 max_frame_bytes: int = MAX_FRAME_BYTES) -> None:
        self.worker = worker
        self.input = input_stream or sys.stdin.buffer
        self.output = output_stream or sys.stdout.buffer
        self.max_frame_bytes = max_frame_bytes
        self._configured = False
        self._active = False
        self._dispatch_local = threading.local()
        self._output_lock = threading.Lock()
        self._host_request_counter = 0
        self._incoming = queue.Queue(maxsize=128)
        self._reply_changed = threading.Condition()
        self._host_replies: Dict[str, Mapping[str, Any]] = {}
        self._waiting_host_requests = set()
        self._input_closed = False
        self._fatal = False
        self._shutdown = False
        self._calls: Dict[str, Dict[str, Any]] = {}
        self._state_versions = {}  # type: Dict[Tuple[str, str], int]
        self.host_request_timeout = 5.0
        worker._server = self

    def _write(self, frame: Mapping[str, Any]) -> None:
        data = json.dumps(frame, separators=(",", ":"), ensure_ascii=True, allow_nan=False).encode() + b"\n"
        if len(data) > self.max_frame_bytes:
            raise ValueError("outgoing frame exceeds limit")
        with self._output_lock:
            self.output.write(data)
            self.output.flush()

    def emit_operation(self, operation_id: str, sequence: int, status: str,
                       payload: Optional[Mapping[str, Any]] = None,
                       message_type: str = "uran.operations.state.v1") -> None:
        if sequence < 1 or status not in TERMINAL_STATES | {"accepted", "running"}:
            raise ValueError("invalid operation event")
        event = {"kind": "event", "event": "operation",
                 "operation_id": operation_id, "sequence": sequence,
                 "status": status, "message_type": message_type,
                 "payload": dict(payload or {})}
        deferred = getattr(self._dispatch_local, "events", None)
        if deferred is not None:
            deferred.append(event)
        else:
            self._write(event)

    def emit_downlink(self, message_type: str, payload: Mapping[str, Any],
                      correlation_id: str = "",
                      target: Optional[Mapping[str, str]] = None) -> None:
        self._write({"kind": "event", "event": "downlink",
                     "message": {"message_type": message_type,
                                 "payload": dict(payload),
                                 "correlation_id": correlation_id,
                                 "target": dict(target or {})}})

    def host_request(self, op: str, **fields: Any) -> Mapping[str, Any]:
        """Run a bounded host request from any worker thread."""
        with self._reply_changed:
            self._host_request_counter += 1
            request_id = f"worker-{self._host_request_counter}"
            self._waiting_host_requests.add(request_id)
        try:
            self._write({"kind": "host_request", "request_id": request_id,
                         "op": op, **fields})
        except Exception:
            with self._reply_changed:
                self._waiting_host_requests.discard(request_id)
            raise
        deadline = time.monotonic() + self.host_request_timeout
        with self._reply_changed:
            while request_id not in self._host_replies and not self._input_closed:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    self._waiting_host_requests.discard(request_id)
                    raise TimeoutError("host request timed out")
                self._reply_changed.wait(remaining)
            self._waiting_host_requests.discard(request_id)
            if request_id not in self._host_replies:
                raise RuntimeError("host reply stream closed")
            reply = self._host_replies.pop(request_id)
        if not reply.get("ok", False):
            error = reply.get("error", {})
            raise RuntimeError(f"{error.get('code', 'host_error')}: "
                               f"{error.get('message', 'host request failed')}")
        return reply

    def call_local(self, message: Mapping[str, Any], timeout: float) -> Mapping[str, Any]:
        operation_id = str(message.get("correlation_id") or "")
        provider = str(message.get("target", {}).get("instance_id") or "")
        if not operation_id or not provider or timeout <= 0:
            raise ValueError("call_local requires correlation_id, target.instance_id and a positive timeout")
        with self._reply_changed:
            if operation_id in self._calls:
                raise ValueError("operation already has a waiting caller")
            call = {"provider": provider, "result": None}
            self._calls[operation_id] = call
        try:
            self.host_request("dispatch", message=dict(message))
            deadline = time.monotonic() + timeout
            with self._reply_changed:
                while call["result"] is None and not self._input_closed:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise TimeoutError(f"operation {operation_id} timed out")
                    self._reply_changed.wait(remaining)
                if call["result"] is None:
                    raise RuntimeError("operation result stream closed")
                result = call["result"]
            if result.get("status") != "succeeded":
                raise RuntimeError(f"operation {operation_id} {result.get('status')}: "
                                   f"{result.get('error', result.get('payload', {}))}")
            return result
        finally:
            with self._reply_changed:
                self._calls.pop(operation_id, None)

    def _on_state_frame(self, frame: Mapping[str, Any]) -> None:
        key = (str(frame["subscription_id"]), str(frame["field"]))
        stamp = int(frame.get("value", {}).get("updated_at_ns", 0))
        if stamp and stamp < self._state_versions.get(key, 0):
            return
        self._state_versions[key] = stamp
        self.worker.on_state(str(frame["subscription_id"]), str(frame["field"]),
                             frame.get("value", {}), int(frame["sequence"]))

    def _enqueue(self, kind: str, item: Any) -> None:
        # Blocking here would prevent the sole reader from delivering replies
        # to a worker waiting inside invoke(), deadlocking both sides.
        self._incoming.put_nowait((kind, item))

    def _read_frames(self) -> None:
        while True:
            raw = self.input.readline(self.max_frame_bytes + 1)
            if not raw:
                break
            if len(raw) > self.max_frame_bytes or not raw.endswith(b"\n"):
                self._enqueue("fatal", "incoming frame exceeds limit")
                break
            try:
                frame = json.loads(raw, parse_constant=lambda value: (_ for _ in ()).throw(
                    ValueError(f"non-finite number: {value}")))
            except Exception as exc:
                self._enqueue("invalid", str(exc))
                continue
            if isinstance(frame, dict) and frame.get("kind") == "host_reply":
                request_id = frame.get("request_id")
                with self._reply_changed:
                    if not isinstance(request_id, str) or request_id not in self._waiting_host_requests:
                        # Timed-out replies can legitimately arrive late.
                        continue
                    self._host_replies[request_id] = frame
                    self._reply_changed.notify_all()
                continue
            if isinstance(frame, dict) and frame.get("kind") == "event" and \
                    frame.get("event") == "operation_result":
                result = frame.get("payload", {})
                source = frame.get("source", {})
                with self._reply_changed:
                    call = self._calls.get(result.get("operation_id"))
                    if call and source.get("instance_id") == call["provider"]:
                        if result.get("status") in TERMINAL_STATES:
                            call["result"] = result
                            self._reply_changed.notify_all()
                        continue
                self._enqueue("operation", frame)
                continue
            if isinstance(frame, dict) and frame.get("kind") == "event" and \
                    frame.get("event") == "state":
                self._enqueue("state", frame)
                continue
            self._enqueue("request", frame)

    def _reader_loop(self) -> None:
        try:
            self._read_frames()
        except Exception:
            self._fatal = True
        finally:
            with self._reply_changed:
                self._input_closed = True
                self._reply_changed.notify_all()
            try:
                self._enqueue("eof", None)
            except queue.Full:
                self._fatal = True

    def _dispatch(self, request: Mapping[str, Any]) -> Dict[str, Any]:
        op = request.get("op")
        if op == "hello":
            if request.get("protocol") != PROTOCOL:
                raise ValueError("unsupported protocol")
            identity = request.get("identity", {})
            host = request.get("host", {})
            if not isinstance(identity, dict) or not isinstance(host, dict):
                raise ValueError("identity and host must be objects")
            self.worker.identity = dict(identity)
            self.worker.host = dict(host)
            self.worker.hello(self.worker.identity, self.worker.host)
            return {"protocol": PROTOCOL, "sdk_version": "1.0",
                    "plugin": {"id": self.worker.plugin_id,
                               "version": self.worker.plugin_version},
                    "capabilities": list(self.worker.capabilities)}
        if op == "configure":
            config = request.get("config", {})
            if not isinstance(config, dict):
                raise ValueError("config must be an object")
            self.worker.configure(config)
            self._configured = True
            return {}
        if op == "activate":
            if not self._configured:
                raise ValueError("worker is not configured")
            self.worker.activate()
            self._active = True
            return {}
        if op == "deactivate":
            if self._active:
                self._active = False
                self.worker.deactivate()
            self._active = False
            return {}
        if op == "invoke":
            if not self._active:
                raise ValueError("worker is not active")
            result = dict(self.worker.invoke(request) or {})
            result.setdefault("status", "accepted")
            return result
        if op == "cancel":
            return dict(self.worker.cancel(str(request["operation_id"])) or {})
        if op == "subscribe":
            self.worker.subscribe(str(request["subscription_id"]),
                                  str(request.get("field_prefix", "")))
            return {}
        if op == "connect":
            self.worker.connect()
            return {}
        if op == "disconnect":
            self.worker.disconnect()
            return {}
        if op == "publish":
            result = dict(self.worker.publish(request.get("message", {})) or {})
            result.setdefault("status", "accepted")
            return result
        if op == "shutdown":
            if self._active:
                self._active = False
                self.worker.deactivate()
            self._shutdown = True
            self.worker.shutdown()
            return {"shutdown": True}
        raise ValueError("unsupported operation")

    def serve(self) -> int:
        try:
            return self._serve()
        finally:
            try:
                if self._active:
                    self._active = False
                    self.worker.deactivate()
            finally:
                if not self._shutdown:
                    self._shutdown = True
                    self.worker.shutdown()

    def _serve(self) -> int:
        reader = threading.Thread(target=self._reader_loop, name="uran-worker-reader", daemon=True)
        reader.start()
        while True:
            if self._fatal:
                return 2
            kind, item = self._incoming.get()
            if kind == "eof":
                break
            if kind == "fatal":
                return 2
            if kind == "state":
                try:
                    self._on_state_frame(item)
                except Exception:
                    return 2
                continue
            if kind == "operation":
                self.worker.on_operation(item.get("payload", {}), item.get("source", {}))
                continue
            if kind == "invalid":
                self._write({"kind": "reply", "request_id": "invalid", "ok": False,
                             "error": Error("invalid_frame", str(item)).as_dict()})
                continue
            request = None
            should_shutdown = False
            try:
                request = item
                if not isinstance(request, dict) or request.get("kind") != "request":
                    raise ValueError("expected request frame")
                request_id = request.get("request_id")
                if not isinstance(request_id, str) or not request_id:
                    raise ValueError("request_id is required")
                self._dispatch_local.events = []
                result = self._dispatch(request)
                reply = {"kind": "reply", "request_id": request_id, "ok": True}
                reply.update(result)
                should_shutdown = request.get("op") == "shutdown"
            except Exception as exc:  # worker errors are protocol results
                request_id = request.get("request_id", "invalid") if isinstance(request, dict) else "invalid"
                reply = {"kind": "reply", "request_id": request_id, "ok": False,
                         "error": Error("worker_error", str(exc)).as_dict()}
            self._write(reply)
            for event in getattr(self._dispatch_local, "events", []):
                self._write(event)
            self._dispatch_local.events = None
            if should_shutdown and reply["ok"]:
                return 0
        return 0
