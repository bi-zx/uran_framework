"""Private Unix socket supervisor; no TCP listener, SSH, SCP or shell API."""
import os
from pathlib import Path
import signal
import socket

from .common import MAX_FRAME, exclusive, loads, read_json
from .engine import DeploymentEngine


def serve(config_path):
    config = read_json(config_path)
    from .bootstrap import validate_config
    validate_config(config)
    if os.geteuid() == 0:
        raise ValueError("run the supervisor as a non-root service account")
    root = Path(config["root"])
    os.umask(0o077)
    (root / "run").mkdir(parents=True, exist_ok=True)
    with exclusive(root / "run/supervisor.lock"):
        engine = DeploymentEngine(config)
        path = root / "run/supervisor.sock"
        if path.exists():
            path.unlink()
        running = [True]
        old_handlers = {}
        for signum in (signal.SIGTERM, signal.SIGINT):
            old_handlers[signum] = signal.signal(signum, lambda *_: running.__setitem__(0, False))
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
                server.bind(str(path))
                path.chmod(0o600)
                server.listen(8)
                server.settimeout(0.2)
                engine.recover()
                while running[0]:
                    engine.maintenance.tick()
                    if (engine.active() and not engine.busy.locked() and not engine.controller.healthy()):
                        # Restore the committed release if Core or a required
                        # dependency dies outside an installation transaction.
                        engine.audit("runtime.restart")
                        engine.restart_managed()
                    try:
                        client, _ = server.accept()
                    except socket.timeout:
                        continue
                    with client:
                        client.settimeout(3)
                        try:
                            frame = bytearray()
                            while b"\n" not in frame:
                                part = client.recv(min(65536, MAX_FRAME + 1 - len(frame)))
                                if not part or len(frame) + len(part) > MAX_FRAME:
                                    raise ValueError("invalid management frame")
                                frame.extend(part)
                            result = engine.submit(loads(bytes(frame).split(b"\n", 1)[0]))
                            response = {"ok": True, "result": result}
                        except Exception as error:
                            response = {"ok": False, "error": {
                                "code": "management_rejected", "message": str(error) if isinstance(error, ValueError)
                                else type(error).__name__}}
                        try:
                            client.sendall(__import__("json").dumps(response, allow_nan=False).encode() + b"\n")
                        except OSError:
                            pass  # The durable task survives a disconnected caller.
        finally:
            engine.maintenance.close()
            engine.close()
            if path.exists():
                path.unlink()
            for signum, handler in old_handlers.items():
                signal.signal(signum, handler)
