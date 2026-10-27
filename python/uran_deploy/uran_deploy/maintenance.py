"""Explicit, expiring human grants gate the optional transport provider."""
import os
from pathlib import Path
import selectors
import subprocess
import sys
import time

from .common import atomic_json, loads, read_json
from .processes import process_identity, terminate


class Maintenance:
    def __init__(self, config, audit):
        self.config = config
        self.audit = audit
        self.path = Path(config["root"]) / "run/maintenance.json"
        self.process = None
        self.record = None
        if self.path.exists():
            old = read_json(self.path)
            if old:
                terminate(old, timeout=2)
            atomic_json(self.path, None)

    def open(self, body, grant):
        config = self.config.get("maintenance", {})
        if not config.get("enabled", False):
            raise ValueError("remote maintenance is disabled by device policy")
        args = body["args"]
        if set(args) != {"service_id", "ssh_public_key"} or args["service_id"] not in config.get("services", {}):
            raise ValueError("maintenance service is not permitted")
        if not isinstance(args["ssh_public_key"], str) or len(args["ssh_public_key"]) > 16384:
            raise ValueError("invalid maintenance SSH public key")
        self.tick()
        if self.record:
            raise ValueError("a maintenance session is already active")
        expires = min(grant["expires_at"], int(time.time()) + config.get("max_session_s", 600))
        if expires <= time.time():
            raise ValueError("maintenance grant expired")
        command = config.get("provider_command")
        if not isinstance(command, list) or not command or not os.path.isabs(command[0]):
            raise ValueError("no locally configured maintenance provider")
        log = Path(self.config["root"]) / "logs/maintenance.log"
        with open(str(log), "ab", buffering=0) as error:
            process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=error, start_new_session=True, bufsize=0)
        record = {"pid": process.pid, "start": process_identity(process.pid),
                  "expires_at": expires, "actor_id": body["actor_id"], "grant_id": grant["grant_id"]}
        try:
            # Persist before the provider may establish a tunnel; recovery closes it.
            atomic_json(self.path, record)
            request = {"service": config["services"][args["service_id"]],
                       "ssh_public_key": args["ssh_public_key"], "expires_at": expires,
                       "session_id": body["request_id"], "parent_pid": os.getpid()}
            process.stdin.write(__import__("json").dumps(request).encode() + b"\n")
            process.stdin.close()
            selector = selectors.DefaultSelector()
            selector.register(process.stdout, selectors.EVENT_READ)
            data = bytearray()
            deadline = time.monotonic() + 10
            try:
                while b"\n" not in data:
                    if time.monotonic() >= deadline or not selector.select(0.2):
                        if time.monotonic() >= deadline:
                            raise ValueError("maintenance provider did not become ready")
                        continue
                    part = os.read(process.stdout.fileno(), 65536)
                    if not part or len(data) + len(part) > 65536:
                        raise ValueError("invalid maintenance provider response")
                    data.extend(part)
            finally:
                selector.close()
            ready = loads(bytes(data).split(b"\n", 1)[0])
            if not ready.get("ok") or process.poll() is not None:
                raise ValueError("maintenance provider failed")
            self.process, self.record = process, record
            self.deadline = time.monotonic() + max(0, expires - time.time())
            self.audit("maintenance.open", grant_id=grant["grant_id"], actor_id=body["actor_id"],
                       expires_at=expires, service_id=args["service_id"])
            return {"session_id": body["request_id"], "expires_at": expires, **ready.get("connection", {})}
        except Exception:
            terminate(record, timeout=2)
            process.wait(timeout=3)
            atomic_json(self.path, None)
            raise

    def tick(self):
        if self.record and (time.time() >= self.record["expires_at"] or
                            time.monotonic() >= self.deadline or self.process.poll() is not None):
            self.close()

    def close(self, grant_id=None, actor=None):
        if not self.record:
            return
        if grant_id is not None and grant_id != self.record["grant_id"]:
            return
        if actor is not None and actor != self.record["actor_id"]:
            raise ValueError("maintenance session belongs to another actor")
        terminate(self.record, timeout=2)
        self.process.wait(timeout=3)
        self.audit("maintenance.closed", grant_id=self.record["grant_id"])
        self.process, self.record = None, None
        atomic_json(self.path, None)
