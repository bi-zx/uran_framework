import io
import json
import sys

sys.path.insert(0, sys.argv[1])
from uran_worker_sdk import Worker, WorkerServer


class Probe(Worker):
    plugin_id = "probe"

    def invoke(self, request):
        return {"status": "accepted", "result": request["payload"]}


frames = [
    {"kind": "request", "request_id": "h", "op": "hello",
     "protocol": "uran.worker.jsonl.v1"},
    {"kind": "request", "request_id": "c", "op": "configure", "config": {}},
    {"kind": "request", "request_id": "a", "op": "activate"},
    {"kind": "request", "request_id": "i", "op": "invoke",
     "operation_id": "op", "payload": {"value": 1}},
    {"kind": "request", "request_id": "x", "op": "cancel",
     "operation_id": "missing"},
]
input_stream = io.BytesIO(b"".join(json.dumps(frame).encode() + b"\n" for frame in frames))
output = io.BytesIO()
assert WorkerServer(Probe(), input_stream, output).serve() == 0
replies = [json.loads(line) for line in output.getvalue().splitlines()]
assert replies[3]["result"] == {"value": 1}
assert replies[4]["ok"] is False

oversized = io.BytesIO(b"{" + b"a" * 100 + b"\n")
assert WorkerServer(Probe(), oversized, io.BytesIO(), max_frame_bytes=32).serve() == 2

malformed_output = io.BytesIO()
assert WorkerServer(Probe(), io.BytesIO(b"[]\n"), malformed_output).serve() == 0
assert json.loads(malformed_output.getvalue())["ok"] is False
