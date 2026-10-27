"""The Core-routed ingress and progress uplink for the independent supervisor."""
import threading

from uran_worker_sdk import Worker, WorkerServer

from .common import rpc


class ManagementWorker(Worker):
    plugin_id = "uran-management"
    plugin_version = "1.0.0"
    capabilities = ("management.device",)

    def configure(self, config):
        self.socket = config["supervisor_socket"]
        self.stop_event = threading.Event()
        self.thread = None

    def activate(self):
        self.stop_event.clear()
        self.thread = threading.Thread(target=self._poll, daemon=True)
        self.thread.start()

    def deactivate(self):
        self.stop_event.set()
        if self.thread:
            self.thread.join(timeout=2)

    def invoke(self, request):
        if request.get("message_type") != "uran.management.command.v1":
            raise ValueError("unsupported management message")
        envelope = request.get("payload", {})
        result = rpc(self.socket, envelope, timeout=10)
        if not result.get("ok"):
            raise ValueError(result.get("error", {}).get("message", "management request rejected"))
        task = result["result"]
        return {"status": "succeeded", "payload": task}

    def _poll(self):
        # Progress is recovered from the durable supervisor on each activation,
        # including after the Core process has upgraded itself. This private
        # local status file is not a cloud file-read API.
        from pathlib import Path
        from .common import digest, read_json
        tasks = Path(self.socket).parent.parent / "tasks"
        seen = {}
        while not self.stop_event.wait(0.5):
            try:
                recovery = read_json(tasks.parent / "run/recovery.json")
                stamp = digest(recovery)
                if seen.get("recovery") != stamp:
                    self.publish_uplink({"message_type": "uran.management.recovery.v1", "payload": recovery})
                    seen["recovery"] = stamp
            except Exception:
                pass
            for path in sorted(tasks.glob("*.json"), key=lambda item: item.stat().st_mtime_ns)[-128:]:
                try:
                    task = read_json(path)
                    # Polling status queries must not recursively generate
                    # their own progress tasks or publish whole artifacts.
                    if task.get("action") not in {"deployment.apply", "deployment.rollback", "deployment.cancel", "maintenance.open", "maintenance.close", "authorization.revoke"}:
                        continue
                    public = {key: task[key] for key in ("request_id", "actor_id", "action", "phase", "status", "updated_at") if key in task}
                    public["schema_version"] = "1.0"
                    for key in ("result", "error"):
                        if key in task:
                            public[key] = task[key]
                    stamp = digest(public)
                    if seen.get(path.name) == stamp:
                        continue
                    self.publish_uplink({"message_type": "uran.management.state.v1", "payload": public,
                        "correlation_id": task["request_id"]})
                    seen[path.name] = stamp
                except Exception:
                    continue


def main():
    return WorkerServer(ManagementWorker()).serve()


if __name__ == "__main__":
    raise SystemExit(main())
