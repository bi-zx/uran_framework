"""Durable, serialized whole-release transactions with compensating recovery."""
import copy
import os
from pathlib import Path
import shutil
import threading
import time

from .artifacts import (download, extract, freeze, platform_check, remove_tree,
                        validate_release, validate_source)
from .common import atomic_json, canonical, digest, identifier, read_json, sync_dir
from .processes import RuntimeController
from .security import Authority

TERMINAL = {"succeeded", "failed", "recovery_failed", "rejected", "cancelled"}


class DeploymentEngine:
    def __init__(self, config, controller=None):
        self.config = config
        self.root = Path(config["root"])
        for name in ("tasks", "releases", "receipts", "staging", "backups", "cache", "run", "logs",
                     "data/state", "data/shared"):
            (self.root / name).mkdir(parents=True, exist_ok=True)
        self.authority = Authority(config, self.root)
        self.controller = controller or RuntimeController(config)
        self.lock = threading.RLock()
        self.busy = threading.Lock()
        self.thread = None
        self.stopping = False
        from .maintenance import Maintenance
        self.maintenance = Maintenance(config, self.audit)

    def audit(self, event, **fields):
        record = dict(fields, event=event, timestamp=int(time.time()))
        with open(str(self.root / "logs/audit.jsonl"), "ab", buffering=0) as output:
            output.write(canonical(record) + b"\n")
            os.fsync(output.fileno())

    def active(self):
        path = self.root / "active.json"
        return read_json(path) if path.exists() else None

    def ledger(self):
        path = self.root / "authorization.json"
        return read_json(path) if path.exists() else {"used": {}, "revoked": []}

    def task(self, request_id):
        path = self.root / "tasks" / (identifier(request_id) + ".json")
        return read_json(path) if path.exists() else None

    def save(self, task, phase=None, **fields):
        with self.lock:
            existing = self.task(task["request_id"])
            if existing and existing.get("cancel_requested"):
                task["cancel_requested"] = True
            task.update(fields)
            if phase:
                task["phase"] = phase
            task["updated_at"] = int(time.time())
            atomic_json(self.root / "tasks" / (task["request_id"] + ".json"), task)
            self.audit("deployment.progress", request_id=task["request_id"],
                       phase=task["phase"], status=task["status"])

    def _current_link(self, release_id):
        temporary = self.root / ".current-next"
        if temporary.is_symlink():
            temporary.unlink()
        os.symlink("releases/" + identifier(release_id), str(temporary))
        os.replace(str(temporary), str(self.root / "current"))
        sync_dir(self.root)

    def _release(self, envelope):
        release, publisher = self.authority.release(envelope)
        validate_source(release["artifact"], self.config.get("policy", {}))
        platform_check(release)
        return release, publisher

    def plan(self, release):
        active = self.active()
        size = release["artifact"]["size"]
        reserve = self.config.get("policy", {}).get("reserve_bytes", 64 * 1024 ** 2)
        if shutil.disk_usage(str(self.root)).free < size * 3 + reserve:
            raise ValueError("insufficient staging space")
        return {"release_id": release["release_id"], "profile_id": release["profile_id"],
                "artifact_sha256": release["artifact"]["sha256"],
                "current_release": active["release_id"] if active else None,
                "steps": ["download", "verify", "stage", "stop", "backup", "migrate",
                          "start", "healthcheck", "commit"],
                "requires_stop": True, "state_schema": release["state_schema"]}

    def submit(self, envelope):
        context = {}
        try:
            body, key = self.authority.command(envelope)
            context = {"actor_id": body["actor_id"], "request_id": body["request_id"], "action": body["action"]}
            with self.lock:
                return self._submit(envelope, body, key)
        except Exception:
            # No raw request, URL, credential, public-key material or arbitrary
            # exception text enters the audit record.
            self.audit("management.rejected", **context)
            raise

    def _submit(self, envelope, body, key):
        action, args = body["action"], body["args"]
        request_id = body["request_id"]
        previous = self.task(request_id)
        if previous:
            if previous["request_sha256"] != digest(body):
                raise ValueError("request ID already belongs to a different request")
            return copy.deepcopy(previous)
        if action == "deployment.status":
            if set(args) != {"request_id"}:
                raise ValueError("status requires a request ID")
            result = self.task(args["request_id"])
            if result is None or (key["role"] != "user" and result["actor_id"] != body["actor_id"]):
                raise ValueError("deployment task is unavailable to this identity")
            return result
        if action == "deployment.cancel":
            if set(args) != {"request_id"}:
                raise ValueError("cancel requires a request ID")
            target = self.task(args["request_id"])
            if (not target or target["action"] not in {"deployment.apply", "deployment.rollback"} or
                    (key["role"] != "user" and target["actor_id"] != body["actor_id"])):
                raise ValueError("deployment cannot be cancelled by this identity")
            if target["status"] in TERMINAL:
                raise ValueError("deployment is already terminal")
            self.save(target, cancel_requested=True)
            return self._record_immediate(body, {"cancel_requested": args["request_id"]})
        if action == "authorization.revoke":
            if key["role"] != "user" or set(args) != {"grant_id"}:
                raise ValueError("only a user can revoke approval")
            grant_id = identifier(args["grant_id"])
            ledger = self.ledger()
            if grant_id not in ledger["revoked"]:
                ledger["revoked"].append(grant_id)
            atomic_json(self.root / "authorization.json", ledger)
            self.maintenance.close(grant_id=grant_id)
            return self._record_immediate(body, {"revoked": grant_id})
        if action == "maintenance.close":
            if args:
                raise ValueError("maintenance.close takes no arguments")
            self.maintenance.close(actor=None if key["role"] == "user" else body["actor_id"])
            return self._record_immediate(body, {"closed": True})
        if action == "maintenance.open":
            if self.busy.locked():
                raise ValueError("remote maintenance cannot open during deployment")
            grant = self.authority.authorize(envelope, body, key, {},
                revoked=self.ledger()["revoked"], used=self.ledger()["used"])
            self._consume(grant, body)
            result = self.maintenance.open(body, grant)
            return self._record_immediate(body, result)
        if action not in {"deployment.plan", "deployment.apply", "deployment.rollback"}:
            raise ValueError("unsupported management action")
        if action == "deployment.rollback":
            if set(args) != {"release_id", "restore_state"} or type(args["restore_state"]) is not bool:
                raise ValueError("rollback requires release_id and explicit restore_state")
            active = self.active()
            previous_release = (active or {}).get("previous")
            if not previous_release or args["release_id"] != previous_release["release_id"]:
                raise ValueError("only the last committed release can be selected for rollback")
            envelope_release = read_json(self.root / "receipts" / (args["release_id"] + ".json"))["envelope"]
            release, _ = self._release(envelope_release)
            publisher_id = envelope_release["key_id"]
            old_schema = active["state_schema"]
            if old_schema != release["state_schema"] and not args["restore_state"]:
                raise ValueError("rollback across state schemas requires explicit snapshot restoration")
            if args["restore_state"] and not active.get("rollback_backup"):
                raise ValueError("no matching state snapshot is available")
        else:
            if set(args) != {"release"}:
                raise ValueError("deployment requires exactly one signed release")
            envelope_release = args["release"]
            release, _ = self._release(envelope_release)
            publisher_id = envelope_release["key_id"]
        plan = self.plan(release)
        if action == "deployment.plan":
            return self._record_immediate(body, plan)
        self.maintenance.tick()
        if self.maintenance.record:
            raise ValueError("close remote maintenance before deploying")
        ledger = self.ledger()
        grant = self.authority.authorize(envelope, body, key, release, publisher_id,
                                          ledger["revoked"], ledger["used"])
        if self.stopping or not self.busy.acquire(blocking=False):
            raise ValueError("another deployment or recovery is in progress")
        try:
            self._consume(grant, body)
            task = {"request_id": request_id, "request_sha256": digest(body),
                    "actor_id": body["actor_id"], "action": action, "status": "accepted",
                    "phase": "accepted", "release": release, "publisher": publisher_id,
                    "previous": self.active(), "backup_ready": False,
                    "grant": grant, "request_expires_at": body["expires_at"],
                    "plan": plan, "restore_state": args.get("restore_state", False)}
            self.save(task)
            self.thread = threading.Thread(target=self._run, args=(task, envelope_release), daemon=True)
            self.thread.start()
            return copy.deepcopy(task)
        except Exception:
            self.busy.release()
            raise

    def _consume(self, grant, body):
        if grant:
            ledger = self.ledger()
            ledger["used"][grant["grant_id"]] = body["request_id"]
            atomic_json(self.root / "authorization.json", ledger)
            self.audit("authorization.used", grant_id=grant["grant_id"], request_id=body["request_id"])

    def _record_immediate(self, body, result):
        task = {"request_id": body["request_id"], "request_sha256": digest(body),
                "actor_id": body["actor_id"], "action": body["action"],
                "phase": "complete", "status": "succeeded", "result": result}
        self.save(task)
        return task

    def _still_authorized(self, task):
        grant = task.get("grant")
        if self.stopping:
            raise ValueError("deployment interrupted for supervisor shutdown")
        if self.task(task["request_id"]).get("cancel_requested", False):
            task["cancel_requested"] = True
            raise ValueError("deployment cancelled")
        if grant:
            self.authority.lifetime(grant, self.config.get("policy", {}).get("max_grant_ttl_s", 900))
            if grant["grant_id"] in self.ledger()["revoked"]:
                raise ValueError("deployment approval has been revoked")
        # Acceptance starts a durable job; the command TTL is an ingress replay
        # window, not the installation timeout. A human grant remains revocable
        # and is rechecked immediately before stopping the active runtime.

    def _stage(self, task, envelope):
        release = task["release"]
        destination = self.root / "releases" / release["release_id"]
        receipt_path = self.root / "receipts" / (release["release_id"] + ".json")
        if destination.exists():
            if not receipt_path.exists() or read_json(receipt_path)["artifact_sha256"] != release["artifact"]["sha256"]:
                raise ValueError("release ID is immutable and already has different content")
            return destination, validate_release(destination, release, self.config.get("policy", {}))
        self.save(task, "downloading", status="running")
        archive = download(release["artifact"], self.root / "cache", self.config.get("policy", {}))
        staging = self.root / "staging" / task["request_id"]
        remove_tree(staging)
        extract(archive, staging, self.config.get("policy", {}))
        lock = validate_release(staging, release, self.config.get("policy", {}))
        freeze(staging)
        # Receipt first: a crash can leave a harmless unused receipt, but never
        # an installed release with no provenance. Directory publication follows.
        atomic_json(receipt_path, {"artifact_sha256": release["artifact"]["sha256"], "envelope": envelope})
        # Moving a directory to another parent updates '..' and needs owner
        # write permission on Linux. Freeze its root again after publication.
        staging.chmod(0o755)
        os.rename(str(staging), str(destination))
        destination.chmod(0o555)
        sync_dir(destination)
        sync_dir(destination.parent)
        return destination, lock

    def _backup(self, task):
        destination = self.root / "backups" / task["request_id"]
        temporary = destination.with_name(destination.name + ".partial")
        remove_tree(temporary)
        if any(path.is_symlink() for path in (self.root / "data/state").rglob("*")):
            raise ValueError("mutable state cannot contain symlinks")
        shutil.copytree(str(self.root / "data/state"), str(temporary))
        # A snapshot is eligible for recovery only after every file and directory
        # has reached stable storage and the ready record is durable.
        for path in temporary.rglob("*"):
            if path.is_symlink():
                raise ValueError("mutable state cannot contain symlinks")
            if path.is_file():
                with open(str(path), "rb") as stream:
                    os.fsync(stream.fileno())
        for path in sorted(temporary.rglob("*"), reverse=True):
            if path.is_dir():
                sync_dir(path)
        sync_dir(temporary)
        os.rename(str(temporary), str(destination))
        sync_dir(destination.parent)
        self.save(task, "backed_up", backup_ready=True)

    def _restore_snapshot(self, snapshot_id):
        snapshot = self.root / "backups" / identifier(snapshot_id)
        if not snapshot.is_dir():
            raise ValueError("state snapshot is unavailable")
        state = self.root / "data/state"
        temporary = self.root / "data/.state-restore"
        remove_tree(temporary)
        shutil.copytree(str(snapshot), str(temporary))
        for path in temporary.rglob("*"):
            if path.is_file():
                with open(str(path), "rb") as stream:
                    os.fsync(stream.fileno())
        for path in sorted(temporary.rglob("*"), reverse=True):
            if path.is_dir():
                sync_dir(path)
        sync_dir(temporary)
        # Repeating this after interruption is safe: the immutable backup remains.
        remove_tree(state)
        os.rename(str(temporary), str(state))
        sync_dir(state.parent)

    def _run(self, task, envelope):
        try:
            destination, lock = self._stage(task, envelope)
            self.save(task, "staged", status="running")
            self._still_authorized(task)
            previous = task["previous"]
            old_schema = previous["state_schema"] if previous else lock["state_schema"]
            if old_schema != lock["state_schema"] and task["action"] != "deployment.rollback":
                if not lock["migration"] or lock["migration"]["from_schema"] != old_schema:
                    raise ValueError("state schema change requires a declared reversible migration")
            self.save(task, "preflight")
            self.controller.variables = {"release": destination, "device_id": self.config["device_id"],
                "data": self.root / "data/state", "shared": self.root / "data/shared", "root": self.root}
            for check in lock["preflight"]:
                self.controller.command(check, destination)
            self._still_authorized(task)
            state_files = list((self.root / "data/state").rglob("*"))
            if any(path.is_symlink() for path in state_files):
                raise ValueError("mutable state cannot contain symlinks")
            state_size = sum(path.stat().st_size for path in state_files if path.is_file())
            reserve = self.config.get("policy", {}).get("reserve_bytes", 64 * 1024 ** 2)
            if shutil.disk_usage(str(self.root)).free < 2 * state_size + reserve:
                raise ValueError("insufficient space for state backup and recovery")
            self.save(task, "stopping")
            self.controller.stop()
            self.save(task, "backing_up")
            self._backup(task)
            self._still_authorized(task)
            if task["restore_state"]:
                self.save(task, "restoring_state")
                self._restore_snapshot(previous["rollback_backup"])
            elif old_schema != lock["state_schema"]:
                self.save(task, "migrating")
                self.controller.variables = {"release": destination,
                    "data": self.root / "data/state", "shared": self.root / "data/shared", "root": self.root}
                self.controller.command({"command": lock["migration"]["command"],
                    "timeout_s": lock["migration"]["timeout_s"]}, destination)
            self._still_authorized(task)
            self.save(task, "switching")
            self._current_link(lock["release_id"])
            self.save(task, "checking")
            self.controller.recovery_mode = False
            self.controller.start(destination, lock)
            self.save(task, "activating")
            self._still_authorized(task)
            self.controller.activate(lambda: self._still_authorized(task))
            with self.lock:
                self._still_authorized(task)
                active = {"release_id": lock["release_id"], "state_schema": lock["state_schema"],
                    "profile_id": lock["profile_id"], "artifact_sha256": task["release"]["artifact"]["sha256"],
                    "previous": ({key: val for key, val in previous.items() if key != "previous"} if previous else None),
                    "rollback_backup": task["request_id"], "request_id": task["request_id"]}
                atomic_json(self.root / "active.json", active)
                self._normal_mode(active)
                self.save(task, "committed", status="succeeded", result={"active": active["release_id"]})
        except Exception as error:
            # Library/URL exceptions must not leak credentials into durable tasks.
            message = str(error) if isinstance(error, ValueError) else type(error).__name__
            try:
                active = self.active()
                if active and active.get("request_id") == task["request_id"]:
                    self.save(task, "committed", status="succeeded", result={"active": active["release_id"]})
                else:
                    self._compensate(task)
                    self.save(task, "rolled_back", status="cancelled" if task.get("cancel_requested") else "failed", error=message)
            except Exception:
                try:
                    self.controller.stop()
                    self._start_management_recovery(self.active(), "rollback_failed")
                except Exception:
                    pass
                self.save(task, "recovery_failed", status="recovery_failed", error=message)
        finally:
            self.busy.release()

    def _compensate(self, task):
        # A staging rejection must not restart or disturb the healthy old host.
        if task["phase"] in {"accepted", "downloading", "staged", "preflight"}:
            return
        self.save(task, "recovering")
        self.controller.stop()
        if task.get("backup_ready"):
            self._restore_snapshot(task["request_id"])
        previous = task.get("previous")
        if previous:
            self._start_active(previous)
        else:
            current = self.root / "current"
            if current.is_symlink():
                current.unlink()
                sync_dir(self.root)

    def _start_active(self, active):
        release_id = active["release_id"]
        receipt = read_json(self.root / "receipts" / (release_id + ".json"))
        release, _ = self.authority.release(receipt["envelope"])
        platform_check(release)
        destination = self.root / "releases" / release_id
        lock = validate_release(destination, release, self.config.get("policy", {}))
        self._current_link(release_id)
        self.controller.recovery_mode = False
        self.controller.start(destination, lock)
        self.controller.activate()
        self._normal_mode(active)

    def _normal_mode(self, active):
        atomic_json(self.root / "run/recovery.json", {"schema_version": "1.0", "mode": "normal",
            "failed_release": "", "running_release": active["release_id"], "reason": "",
            "product_runtime_started": active["profile_id"] != "management"})

    def _start_management_recovery(self, active, reason):
        """An empty, signed Core keeps cloud repair possible without SSH.

        It has separate runtime state and never migrates/rewinds product data.
        The desired committed selection remains in active.json/current.
        """
        prefix = Path(self.config["bootstrap_prefix"])
        envelope = read_json(prefix / "seed.tar.gz.release.json")
        release, _ = self.authority.release(envelope)
        if release["profile_id"] != "management":
            raise ValueError("bootstrap recovery profile is not management-only")
        destination = self.root / "recovery" / release["release_id"]
        destination.parent.mkdir(parents=True, exist_ok=True)
        try:
            lock = validate_release(destination, release, self.config.get("policy", {}))
        except (OSError, ValueError):
            remove_tree(destination)
            archive = prefix / "seed.tar.gz"
            from .common import file_hash
            if archive.stat().st_size != release["artifact"]["size"] or file_hash(archive) != release["artifact"]["sha256"]:
                raise ValueError("trusted bootstrap recovery archive is unavailable")
            extract(archive, destination, self.config.get("policy", {}))
            lock = validate_release(destination, release, self.config.get("policy", {}))
            freeze(destination)
        runtime = read_json(destination / lock["runtime_config"])
        if runtime.get("plugins") or lock["services"] or lock["checks"] or lock["preflight"] or lock["post_activate"] or lock["migration"]:
            raise ValueError("recovery profile must contain no product code")
        record = {"schema_version": "1.0", "mode": "management_recovery",
            "failed_release": active["release_id"] if active else "",
            "running_release": release["release_id"], "reason": reason,
            "product_runtime_started": False}
        atomic_json(self.root / "run/recovery.json", record)
        self.audit("runtime.management_recovery", **record)
        self.controller.recovery_mode = True
        self.controller.start(destination, lock)
        self.controller.activate()

    def restart_managed(self):
        active = self.active()
        self.controller.stop()
        path = self.root / "run/recovery.json"
        mode = read_json(path) if path.exists() else {}
        if mode.get("mode") == "management_recovery" and mode.get("failed_release") == active["release_id"]:
            self._start_management_recovery(active, mode["reason"])
            return
        try:
            self._start_active(active)
        except Exception:
            self.controller.stop()
            self._start_management_recovery(active, "committed_release_unavailable")

    def recover(self):
        """Called with the exclusive supervisor lock, before accepting tasks."""
        self.controller.recover_processes()
        active = self.active()
        interrupted = [read_json(path) for path in sorted((self.root / "tasks").glob("*.json"))
                       if read_json(path)["status"] not in TERMINAL]
        # Serial execution permits at most one pending deployment.
        for task in interrupted:
            if active and active.get("request_id") == task["request_id"]:
                self.save(task, "committed", status="succeeded", result={"active": active["release_id"]})
            else:
                if task.get("backup_ready"):
                    self._restore_snapshot(task["request_id"])
                self.save(task, "interrupted", status="failed", error="supervisor restarted; previous committed release selected")
        if active:
            self.restart_managed()
            mode = read_json(self.root / "run/recovery.json")
            if mode["mode"] == "management_recovery":
                for task in interrupted:
                    if task["status"] != "succeeded":
                        self.save(task, "recovery_failed", status="recovery_failed")

    def close(self):
        self.stopping = True
        if self.thread and self.thread.is_alive():
            # Do not stop the host concurrently with a transaction. systemd's
            # TimeoutStopSec provides an outer bound for blocking I/O.
            self.thread.join(timeout=self.config.get("shutdown_timeout_s", 45))
            if self.thread.is_alive():
                return
        self.controller.stop()
