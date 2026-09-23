import json
import selectors
import subprocess
import sys
import tempfile
import time
from pathlib import Path

runtime, manifest, worker = sys.argv[1:]

for mode in ("malformed", "timeout", "crash"):
    with tempfile.TemporaryDirectory() as directory:
        config = Path(directory) / "runtime.yaml"
        config.write_text(f"""
plugins:
  - instance_id: worker
    device_id: robot
    component_id: base
    class_type: uran_worker_adapter/WorkerProcessPlugin
    config:
      command: [{json.dumps(sys.executable)}, {json.dumps(worker)}, {mode}]
      message_types: [uran.test.invoke.v1]
      capabilities: [probe]
      request_timeout_ms: 120
      shutdown_timeout_ms: 200
""")
        process = subprocess.Popen(
            [runtime, "--manifest", manifest, "--config", str(config)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, bufsize=1)
        selector = selectors.DefaultSelector()
        selector.register(process.stdout, selectors.EVENT_READ)

        def request(frame, timeout=3):
            process.stdin.write(json.dumps(frame) + "\n")
            process.stdin.flush()
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                for key, _ in selector.select(0.05):
                    line = key.fileobj.readline()
                    if not line:
                        raise AssertionError(f"runtime exited for {mode}: {process.stderr.read()}")
                    value = json.loads(line)
                    if "ok" in value:
                        return value
            raise AssertionError(f"runtime request timed out for {mode}")

        queued = request({"op": "dispatch", "message_type": "uran.test.invoke.v1",
                          "payload": {}, "correlation_id": mode,
                          "target": {"instance_id": "worker"}})
        assert queued["ok"], queued
        time.sleep(0.35)
        status = request({"op": "status"})
        assert status["ok"], status
        plugin = status["result"]["plugins"][0]
        assert plugin["health"] in (4, 5), plugin
        stopped = request({"op": "shutdown"})
        assert stopped["ok"], stopped
        process.stdin.close()
        assert process.wait(timeout=3) == 0, process.stderr.read()
