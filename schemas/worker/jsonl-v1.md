# URAN worker JSONL v1

`uran.worker.jsonl.v1` is the process boundary for Python/vendor workers. The
transport is UTF-8 JSON, exactly one object per line. Stdout is reserved for
protocol frames and stderr for logs. A frame, including its newline, is limited
to 1 MiB by default and both peers must reject larger frames.

The host starts a worker without a shell from the configured `command` array.
It sends `hello`, `configure`, `activate`, zero or more `subscribe` requests,
then `invoke`/`cancel`. Shutdown is `deactivate` followed by `shutdown`. Every
host command has a unique string `request_id`; the worker returns exactly one
reply with the same ID before emitting events caused by that command.

```json
{"kind":"request","protocol":"uran.worker.jsonl.v1","request_id":"r1","op":"hello","identity":{"instance_id":"base","device_id":"robot","component_id":"base"},"host":{"sdk_version":"2.1","max_frame_bytes":1048576}}
{"kind":"reply","request_id":"r1","ok":true,"protocol":"uran.worker.jsonl.v1","sdk_version":"1.0","plugin":{"id":"cyberdog2-motion","version":"1.0.0"},"capabilities":["motion.base.velocity"]}
{"kind":"request","request_id":"r2","op":"configure","config":{}}
{"kind":"request","request_id":"r3","op":"activate"}
{"kind":"request","request_id":"r4","op":"subscribe","subscription_id":"state-1","field_prefix":"device.base."}
{"kind":"request","request_id":"r5","op":"invoke","operation_id":"op-7","message_type":"uran.motion.command.v1","target":{"device_id":"robot","component_id":"base","instance_id":"","capability_id":"motion.base.velocity"},"payload":{},"deadline_ms":5000,"correlation_id":"cloud-9"}
{"kind":"reply","request_id":"r5","ok":true,"status":"accepted","operation_id":"op-7"}
{"kind":"event","event":"operation","operation_id":"op-7","sequence":1,"status":"running","message_type":"uran.operations.state.v1","payload":{}}
{"kind":"event","event":"operation","operation_id":"op-7","sequence":2,"status":"succeeded","message_type":"uran.operations.state.v1","payload":{}}
{"kind":"request","request_id":"r6","op":"cancel","operation_id":"op-7"}
```

Allowed operation states are `accepted`, `running`, `succeeded`, `failed`, and
`cancelled`. Sequence starts at one per operation and strictly increases.
`succeeded`, `failed`, and `cancelled` are terminal. Cancel is idempotent:
repeated cancel requests return success with the current terminal status.

An error reply is always typed:

```json
{"kind":"reply","request_id":"r6","ok":false,"error":{"code":"not_found","message":"unknown operation","retryable":false,"details":{}}}
```

The worker may call another local plugin or update its scoped state by emitting
a `host_request`. The host handles it asynchronously on the Runtime executor
and returns a reply. A worker must send the reply for the current host command
before waiting for a `host_request`; violating this order is rejected by the
host command timeout rather than blocking the Runtime indefinitely.

```json
{"kind":"host_request","request_id":"w1","op":"dispatch","message":{"message_type":"uran.motion.command.v1","payload":{},"correlation_id":"nested","target":{"device_id":"robot","component_id":"arm","instance_id":"","capability_id":"motion.arm.pose"}}}
{"kind":"host_request","request_id":"w2","op":"state.set","field":"plugin.base.progress","value":0.5,"persistent":false}
{"kind":"host_reply","request_id":"w1","ok":true}
```

State notifications use `kind=event`, `event=state`, `subscription_id`,
`sequence`, `field`, and a `value` object containing `value`, `source`,
`updated_at_ms`, and `persistent`. The host preserves ordering per process.

EOF, invalid JSON, oversized frames, handshake mismatch, request timeout, or a
nonzero exit marks the bridge unhealthy. Timeout never means success. During
cleanup the host closes stdin, sends SIGTERM to the worker process group, waits
for the configured grace period, and uses SIGKILL only if it remains alive.

The reference Python SDK import is `uran_worker_sdk`; its CLI is:

```bash
python3 -m uran_worker_sdk package.module:worker_factory
```

The factory returns a `uran_worker_sdk.Worker` implementation. It has no ROS or
URAN Runtime dependency and may be installed into the plugin's locked venv.
