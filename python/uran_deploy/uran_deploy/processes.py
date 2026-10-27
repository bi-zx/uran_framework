"""Supervised host and dependency services; never accepts a remote command."""
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

from .artifacts import validate_services
from .common import atomic_json, read_json, relative_path, rpc


def expand(value, variables):
    if isinstance(value, dict):
        return {key: expand(item, variables) for key, item in value.items()}
    if isinstance(value, list):
        return [expand(item, variables) for item in value]
    if isinstance(value, str):
        for key, item in variables.items():
            value = value.replace("${" + key + "}", str(item))
    return value


def process_identity(pid):
    try:
        # comm may contain spaces and parentheses. Field 22 follows its last ')'.
        return Path("/proc/%d/stat" % pid).read_text().rsplit(")", 1)[1].split()[19]
    except (OSError, IndexError):
        return None


def terminate(record, timeout=10):
    pid = record.get("pid")
    if not pid or not record.get("start") or process_identity(pid) != record["start"]:
        return
    try:
        os.killpg(pid, signal.SIGTERM)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline and process_identity(pid) == record["start"]:
            # Reap direct children, including those left in a previous controller.
            try:
                if os.waitpid(pid, os.WNOHANG)[0]:
                    return
            except ChildProcessError:
                pass
            time.sleep(0.05)
        if process_identity(pid) == record["start"]:
            os.killpg(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


class RuntimeController:
    def __init__(self, config):
        self.config = config
        self.root = Path(config["root"])
        self.prefix = Path(config["bootstrap_prefix"])
        self.run = self.root / "run"
        self.run.mkdir(parents=True, exist_ok=True)
        self.socket = self.run / "core.sock"
        self.processes = []
        self.variables = {}
        self.recovery_mode = False
        self.pending_activation = []
        self.current_release = None
        self.current_lock = None

    def state_directory(self):
        path = self.root / "data" / ("recovery" if self.recovery_mode else "state")
        path.mkdir(parents=True, exist_ok=True)
        return path

    def _environment(self):
        # Never inherit interactive PYTHONPATH/LD_PRELOAD/PYTHONHOME.
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(("PYTHON", "LD_"))}
        env["PYTHONDONTWRITEBYTECODE"] = "1"
        env["URAN_DATA_DIR"] = str(self.state_directory())
        env["URAN_SHARED_DIR"] = str(self.root / "data" / "shared")
        env["URAN_CORE_SOCKET"] = str(self.socket)
        return env

    def _spawn(self, name, argv, cwd):
        log = self.root / "logs" / (name + ".log")
        log.parent.mkdir(parents=True, exist_ok=True)
        with open(str(log), "ab", buffering=0) as output:
            process = subprocess.Popen([self.config.get("python", sys.executable),
                str(self.prefix / "bin" / "uran-exec"), str(os.getpid())] + argv,
                stdin=subprocess.DEVNULL, stdout=output, stderr=output,
                cwd=str(cwd), env=self._environment(), start_new_session=True)
        record = {"pid": process.pid, "start": process_identity(process.pid), "name": name}
        self.processes.append((process, record))
        atomic_json(self.run / "processes.json", [entry for _, entry in self.processes])
        return process

    def stop(self):
        # Host stops first, releasing all hardware controllers before services.
        for process, record in reversed(self.processes):
            terminate(record, self.config.get("stop_timeout_s", 12))
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                raise ValueError("managed process failed to stop")
        self.processes = []
        atomic_json(self.run / "processes.json", [])
        if self.socket.exists():
            self.socket.unlink()

    def recover_processes(self):
        path = self.run / "processes.json"
        if path.exists():
            for record in reversed(read_json(path)):
                terminate(record, self.config.get("stop_timeout_s", 12))
        atomic_json(path, [])
        if self.socket.exists():
            self.socket.unlink()

    def command(self, spec, release):
        argv = expand(spec["command"], self.variables)
        argv[0] = str(relative_path(release, spec["command"][0]))
        process = self._spawn("checks", argv, release)
        try:
            try:
                code = process.wait(timeout=spec["timeout_s"])
            except subprocess.TimeoutExpired:
                terminate(self.processes[-1][1], timeout=12)
                process.wait(timeout=2)
                raise ValueError("release check or migration timed out")
            if code:
                raise ValueError("release check or migration failed (see local checks.log)")
        finally:
            self.processes = [(child, record) for child, record in self.processes if child is not process]
            atomic_json(self.run / "processes.json", [record for _, record in self.processes])

    def _config(self, release, lock):
        variables = {"release": release, "device_id": self.config["device_id"], "data": self.state_directory(),
                     "shared": self.root / "data" / "shared", "root": self.root}
        self.variables = variables
        runtime = expand(read_json(release / lock["runtime_config"]), variables)
        device = self.config["device_id"]
        runtime["default_device_id"] = device
        runtime["primary_protocol_instance"] = "uran_cloud"
        runtime.setdefault("plugin_manifests", []).append(str(
            self.prefix / "sdk/share/uran_framework/plugins/worker/plugin.json"))
        plugins = runtime.setdefault("plugins", [])
        self.pending_activation = []
        for item in sorted(plugins, key=lambda entry: entry.get("priority", 0), reverse=True):
            if item.get("enabled", True) and item.get("auto_activate", True):
                self.pending_activation.append(item["instance_id"])
                item["auto_activate"] = False
        cloud = dict(self.config.get("cloud", {}))
        cloud.update(device_id=device, management_instance_id="uran_management")
        common = {"device_id": device, "component_id": "management", "api_version": "2.1"}
        plugins.append(dict(common, instance_id="uran_management", plugin_type="worker",
            class_type="uran_worker_adapter/WorkerProcessPlugin", config={
                "command": [self.config.get("python", sys.executable), str(self.prefix / "bin/uran-management-worker")],
                "message_types": ["uran.management.command.v1"],
                "capabilities": ["management.device"], "request_timeout_ms": 15000,
                "worker_config": {"supervisor_socket": str(self.run / "supervisor.sock")}}))
        if cloud.get("transport_enabled", True):
            plugins.append(dict(common, instance_id="uran_cloud", plugin_type="protocol",
                class_type="uran_worker_adapter/WorkerProtocolPlugin", priority=100,
                config={"command": [self.config.get("python", sys.executable), str(self.prefix / "bin/uran-cloud-worker")],
                        "request_timeout_ms": 20000, "state_subscriptions": [""],
                        "worker_config": cloud}))
        else:
            runtime.pop("primary_protocol_instance")
        path = self.run / "runtime.json"
        atomic_json(path, runtime)
        return path

    def start(self, release, lock):
        release = Path(release)
        self.current_release, self.current_lock = release, lock
        config_path = self._config(release, lock)
        services = {entry["id"]: entry for entry in lock["services"]}
        for name in validate_services(lock["services"], release):
            spec = services[name]
            argv = expand(spec["command"], self.variables)
            argv[0] = str(relative_path(release, spec["command"][0]))
            self._spawn(name, argv, release)
            self.command(spec["healthcheck"], release)
        self._spawn("core", [str(release / lock["host"]), "--config", str(config_path),
            "--state-file", str(self.state_directory() / "runtime.json"),
            "--control-socket", str(self.socket)], release)
        deadline = time.monotonic() + self.config.get("health_timeout_s", 45)
        last = "runtime has not started"
        while time.monotonic() < deadline:
            if any(process.poll() is not None for process, _ in self.processes):
                raise ValueError("a managed runtime or dependency service exited")
            try:
                reply = rpc(self.socket, {"op": "status"}, timeout=1)
                if reply.get("ok"):
                    status = reply["result"]
                    plugins = status.get("plugins", [])
                    required = {"uran_management"}
                    if self.config.get("cloud", {}).get("transport_enabled", True):
                        required.add("uran_cloud")
                    # Diagnostics represent plugin state using the public API.
                    active = {item["instance_id"] for item in plugins
                              if item.get("state") == "active"}
                    if required <= active:
                        break
                    last = "required management plugins are not active"
            except (OSError, ValueError, KeyError):
                pass
            time.sleep(0.1)
        else:
            raise ValueError(last)
        for check in lock["checks"]:
            self.command(check, release)
        if any(process.poll() is not None for process, _ in self.processes):
            raise ValueError("a managed service exited during health checks")

    def activate(self, guard=None):
        for instance_id in self.pending_activation:
            if guard:
                guard()
            result = rpc(self.socket, {"op": "activate", "instance_id": instance_id}, timeout=20)
            if not result.get("ok"):
                raise ValueError("candidate plugin activation failed")
        for check in self.current_lock["post_activate"]:
            if guard:
                guard()
            self.command(check, self.current_release)
        if any(process.poll() is not None for process, _ in self.processes):
            raise ValueError("a managed service exited after activation")

    def healthy(self):
        return bool(self.processes) and all(process.poll() is None for process, _ in self.processes)
