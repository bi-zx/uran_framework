import json
import os
import selectors
import subprocess
import sys
import tempfile
import time
from pathlib import Path


runtime, manifest, probe = sys.argv[1:]
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    config = root / "runtime.yaml"
    config.write_text(f"""
plugins:
  - instance_id: worker
    device_id: robot
    component_id: base
    class_type: uran_worker_adapter/WorkerProcessPlugin
    config:
      command: [{json.dumps(sys.executable)}, {json.dumps(probe)}]
      message_types: [uran.test.invoke.v1, uran.operations.cancel.v1]
      capabilities: [probe.echo]
      request_timeout_ms: 1000
""")
    process = subprocess.Popen(
        [runtime, "--manifest", manifest, "--config", str(config)],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, bufsize=1)
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)

    def request(frame):
        process.stdin.write(json.dumps(frame) + "\n")
        process.stdin.flush()
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            for key, _ in selector.select(0.1):
                value = json.loads(key.fileobj.readline())
                if "ok" in value:
                    return value
        raise AssertionError("runtime request timed out")

    start = time.monotonic()
    result = request({"op": "dispatch", "message_type": "uran.test.invoke.v1",
                      "payload": {"value": 7}, "correlation_id": "operation-1",
                      "target": {"instance_id": "worker"}})
    assert result["ok"], result
    assert time.monotonic() - start < 0.5, "dispatch waited for worker completion"
    deadline = time.monotonic() + 3
    snapshot = None
    while time.monotonic() < deadline:
        snapshot = request({"op": "snapshot"})
        if snapshot.get("result", {}).get("plugin.worker.last_value") == {"value": 7}:
            break
        time.sleep(0.05)
    assert snapshot["result"]["plugin.worker.last_value"] == {"value": 7}
    assert request({"op": "shutdown"})["ok"]
    process.stdin.close()
    assert process.wait(timeout=5) == 0, process.stderr.read()
