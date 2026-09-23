#!/usr/bin/env python3
import json
import os
import sys
import time


def send(frame):
    sys.stdout.write(json.dumps(frame, separators=(",", ":")) + "\n")
    sys.stdout.flush()


def main():
    terminal = {}
    host_reply_seen = False
    for line in sys.stdin:
        frame = json.loads(line)
        if frame.get("kind") == "host_reply":
            host_reply_seen = frame.get("ok", False)
            continue
        request_id = frame["request_id"]
        op = frame["op"]
        if op == "hello":
            send({"kind": "reply", "request_id": request_id, "ok": True,
                  "protocol": "uran.worker.jsonl.v1", "sdk_version": "1.0",
                  "plugin": {"id": "probe", "version": "1.0"},
                  "capabilities": ["probe.echo"]})
        elif op in ("configure", "activate", "deactivate", "subscribe"):
            send({"kind": "reply", "request_id": request_id, "ok": True})
        elif op == "invoke":
            operation_id = frame["operation_id"]
            send({"kind": "reply", "request_id": request_id, "ok": True,
                  "status": "accepted", "operation_id": operation_id,
                  "result": {"preserved": frame["payload"]}})
            send({"kind": "host_request", "request_id": "worker-local",
                  "op": "state.set", "field": "plugin.worker.last_value",
                  "value": frame["payload"], "persistent": False})
            deadline = time.monotonic() + 2.0
            while not host_reply_seen and time.monotonic() < deadline:
                nested = json.loads(sys.stdin.readline())
                if nested.get("kind") == "host_reply":
                    host_reply_seen = nested.get("ok", False)
            status = "succeeded" if host_reply_seen else "failed"
            terminal[operation_id] = status
            send({"kind": "event", "event": "operation", "operation_id": operation_id,
                  "sequence": 1, "status": status,
                  "message_type": "uran.operations.state.v1",
                  "payload": {"host_reply": host_reply_seen}})
        elif op == "cancel":
            operation_id = frame["operation_id"]
            status = terminal.get(operation_id)
            if status:
                send({"kind": "reply", "request_id": request_id, "ok": True,
                      "status": status, "operation_id": operation_id})
            else:
                send({"kind": "reply", "request_id": request_id, "ok": False,
                      "error": {"code": "not_found", "message": "unknown operation",
                                "retryable": False, "details": {}}})
        elif op == "shutdown":
            send({"kind": "reply", "request_id": request_id, "ok": True,
                  "shutdown": True})
            return 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
