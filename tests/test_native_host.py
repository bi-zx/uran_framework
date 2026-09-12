import json
import subprocess
import sys

runtime, manifest, config = sys.argv[1:]
commands = [
    {"op": "status"},
    {"op": "deactivate", "instance_id": "protocol_primary"},
    {"op": "publish", "message_type": "framework.echo", "payload": {"value": 1}, "correlation_id": "test"},
    {"op": "snapshot"},
    {"op": "unload", "instance_id": "protocol_backup"},
    {"op": "status"},
]
result = subprocess.run([runtime, "--manifest", manifest, "--config", config],
                        input="".join(json.dumps(c) + "\n" for c in commands),
                        text=True, capture_output=True, timeout=15, check=True)
messages = [json.loads(line) for line in result.stdout.splitlines()]
responses = [m for m in messages if "ok" in m]
assert len(responses) == len(commands), messages
assert all(m["ok"] for m in responses), responses
assert responses[0]["result"]["protocol_instance_count"] == 2
assert responses[-1]["result"]["protocol_instance_count"] == 0
event = next(m for m in messages if m.get("event") == "downlink")
assert event["source"]["instance_id"] == "protocol_backup"
assert event["correlation_id"] == "test"
assert event["payload"] == {"value": 1}
print("Native host: lifecycle, protocol fallback and source propagation passed")
