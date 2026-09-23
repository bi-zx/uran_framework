#!/usr/bin/env python3
import json
import os
import sys
import time

mode = sys.argv[1]
for line in sys.stdin:
    request = json.loads(line)
    op = request.get("op")
    request_id = request.get("request_id")
    if mode == "malformed" and op == "invoke":
        print(json.dumps({"kind": 7, "request_id": request_id}), flush=True)
        time.sleep(5)
        continue
    if mode == "timeout" and op == "invoke":
        time.sleep(5)
        continue
    if mode == "crash" and op == "invoke":
        os._exit(23)
    if op == "hello":
        reply = {"kind": "reply", "request_id": request_id, "ok": True,
                 "protocol": "uran.worker.jsonl.v1", "sdk_version": "1.0",
                 "plugin": {"id": mode, "version": "1.0"}, "capabilities": ["probe"]}
    else:
        reply = {"kind": "reply", "request_id": request_id, "ok": True}
        if op in ("invoke", "cancel", "publish"):
            reply.update(status="accepted", operation_id=request.get("operation_id"))
        if op == "shutdown":
            print(json.dumps(reply), flush=True)
            break
    print(json.dumps(reply), flush=True)
