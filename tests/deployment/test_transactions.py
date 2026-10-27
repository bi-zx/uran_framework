import copy
import io
import os
from pathlib import Path
import platform
import tarfile
import time

import pytest

from uran_deploy.artifacts import download, extract, validate_source
from uran_deploy.common import atomic_json, digest, read_json
from uran_deploy.engine import DeploymentEngine
from uran_deploy.packaging import build_release
from uran_deploy.security import Authority, sign


def command(keys, action, args, request_id="request-1", role="agent"):
    now = int(time.time())
    return sign({"schema_version": "1.0", "kind": "command", "request_id": request_id,
        "device_id": "test_device", "actor_id": role, "issued_at": now,
        "expires_at": now + 300, "action": action, "args": args}, keys[role][0], role)


def approval(keys, envelope, grant_id="grant-1", ttl=300):
    body = envelope["payload"]
    now = int(time.time())
    envelope["grant"] = sign({"schema_version": "1.0", "kind": "grant", "grant_id": grant_id,
        "device_id": body["device_id"], "actor_id": body["actor_id"], "request_sha256": digest(body),
        "issued_at": now, "expires_at": now + ttl}, keys["user"][0], "user")
    return envelope


def release_tree(root, keys, https, release_id, state_schema="1", migration=None):
    source = root / (release_id + "-source")
    (source / "bin").mkdir(parents=True)
    host = source / "bin/host"
    host.write_text("#!/bin/sh\nexit 0\n")
    host.chmod(0o755)
    (source / "runtime.json").write_text('{"plugins":[]}')
    spec = {"release_id": release_id, "profile_id": "test", "profile_version": release_id,
            "api_version": "2.1", "state_schema": state_schema,
            "platform": {"os": platform.system(), "arch": platform.machine()},
            "host": "bin/host", "runtime_config": "runtime.json", "migration": migration,
            "packages": [{"package": "test", "version": release_id, "origin": "test:fixture", "sha256": "a" * 64}]}
    return build_release(source, spec, https["root"] / (release_id + ".tar.gz"),
        https["url"] + "/" + release_id + ".tar.gz", keys["publisher"][0], "publisher")


class Controller:
    def __init__(self, config):
        self.config = config
        self.current = None
        self.stops = 0
        self.fail_release = None

    def start(self, root, lock):
        if lock["release_id"] == self.fail_release:
            (Path(self.config["root"]) / "data/state/changed").write_text("bad-state")
            raise ValueError("injected healthcheck failure")
        self.current = lock["release_id"]

    def stop(self):
        self.stops += 1
        self.current = None

    def recover_processes(self):
        self.stop()

    def activate(self, guard=None):
        if guard:
            guard()

    def command(self, spec, root):
        (Path(self.config["root"]) / "data/state/migrated").write_text("new schema")


def configured(config, https):
    config["policy"].update(artifact_origins=[https["url"]], artifact_ca_file=https["ca"])
    return config


def finish(engine, request_id):
    engine.thread.join(timeout=20)
    assert not engine.thread.is_alive()
    return engine.task(request_id)


def test_success_health_failure_and_explicit_rollback(config, keys, https, tmp_path):
    configured(config, https)
    controller = Controller(config)
    engine = DeploymentEngine(config, controller)
    first = release_tree(tmp_path, keys, https, "v1")
    envelope = command(keys, "deployment.apply", {"release": first})
    engine.submit(envelope)
    assert finish(engine, "request-1")["status"] == "succeeded"
    assert engine.active()["release_id"] == "v1"
    state = Path(config["root"]) / "data/state/original"
    state.write_text("preserved")
    shared = Path(config["root"]) / "data/shared/calibration"
    shared.write_text("calibrated")
    controller.fail_release = "v2"
    second = release_tree(tmp_path, keys, https, "v2")
    engine.submit(command(keys, "deployment.apply", {"release": second}, "request-2"))
    failed = finish(engine, "request-2")
    assert failed["status"] == "failed" and failed["phase"] == "rolled_back"
    assert engine.active()["release_id"] == controller.current == "v1"
    assert state.read_text() == "preserved"
    assert not (state.parent / "changed").exists()
    assert shared.read_text() == "calibrated"
    controller.fail_release = None
    engine.submit(command(keys, "deployment.apply", {"release": second}, "request-3"))
    assert finish(engine, "request-3")["status"] == "succeeded"
    rollback = command(keys, "deployment.rollback", {"release_id": "v1", "restore_state": False}, "request-4")
    with pytest.raises(ValueError):
        engine.submit(rollback)
    approval(keys, rollback)
    engine.submit(rollback)
    assert finish(engine, "request-4")["status"] == "succeeded"
    assert controller.current == engine.active()["release_id"] == "v1"
    assert engine.submit(envelope)["status"] == "succeeded", "duplicate delivery must not reinstall"


def test_signatures_roles_grant_scope_replay_and_revocation(config, keys, https, tmp_path):
    configured(config, https)
    config["policy"]["auto_deploy"] = []
    engine = DeploymentEngine(config, Controller(config))
    release = release_tree(tmp_path, keys, https, "v1")
    request = command(keys, "deployment.apply", {"release": release})
    with pytest.raises(ValueError):
        engine.submit(request)
    tampered = copy.deepcopy(request)
    tampered["payload"]["actor_id"] = "user"
    with pytest.raises(ValueError):
        engine.submit(tampered)
    approval(keys, request)
    different = command(keys, "deployment.apply", {"release": release}, "different")
    different["grant"] = request["grant"]
    with pytest.raises(ValueError, match="concrete request"):
        engine.submit(different)
    engine.submit(command(keys, "authorization.revoke", {"grant_id": "grant-1"}, "revoke", "user"))
    with pytest.raises(ValueError, match="revoked"):
        engine.submit(request)
    approval(keys, request, grant_id="grant-2")
    engine.submit(request)
    assert finish(engine, "request-1")["status"] == "succeeded"
    changed = command(keys, "deployment.apply", {"release": release}, "request-1")
    changed["payload"]["expires_at"] -= 1
    changed = sign(changed["payload"], keys["agent"][0], "agent")
    with pytest.raises(ValueError, match="different request"):
        engine.submit(changed)
    wrong_publisher = sign(release["payload"], keys["agent"][0], "agent")
    with pytest.raises(ValueError, match="role"):
        engine.submit(command(keys, "deployment.plan", {"release": wrong_publisher}, "bad-publisher"))


def test_expired_wrong_device_and_legacy_32bit_times_rejected(config, keys):
    authority = Authority(config, config["root"])
    request = command(keys, "deployment.status", {"request_id": "x"})
    body = request["payload"]
    for changes in ({"device_id": "other"}, {"expires_at": int(time.time()) - 1},
                    {"issued_at": -1208728896, "expires_at": -1208728895}):
        with pytest.raises(ValueError):
            authority.command(sign(dict(body, **changes), keys["agent"][0], "agent"))


def test_restart_restores_snapshot_before_committed_release(config, keys, https, tmp_path):
    configured(config, https)
    engine = DeploymentEngine(config, Controller(config))
    first = release_tree(tmp_path, keys, https, "v1")
    engine.submit(command(keys, "deployment.apply", {"release": first}))
    assert finish(engine, "request-1")["status"] == "succeeded"
    state = Path(config["root"]) / "data/state/value"
    state.write_text("old")
    task = {"request_id": "crashed", "request_sha256": "b" * 64, "actor_id": "agent",
            "status": "running", "phase": "checking", "backup_ready": False, "previous": engine.active()}
    engine._backup(task)
    state.write_text("corrupted-by-new-release")
    task["phase"] = "checking"
    engine.save(task)
    recovered = DeploymentEngine(config, Controller(config))
    recovered.recover()
    assert recovered.task("crashed")["status"] == "failed"
    assert state.read_text() == "old" and recovered.controller.current == "v1"


def test_hash_archive_and_origin_fail_before_stopping(config, keys, https, tmp_path):
    configured(config, https)
    controller = Controller(config)
    engine = DeploymentEngine(config, controller)
    release = release_tree(tmp_path, keys, https, "v1")
    (https["root"] / "v1.tar.gz").write_bytes(b"corrupt")
    engine.submit(command(keys, "deployment.apply", {"release": release}))
    assert finish(engine, "request-1")["status"] == "failed"
    assert controller.stops == 0
    for url in ("http://localhost/v1.tar.gz", "https://elsewhere.invalid/v1", "https://user:password@localhost/a"):
        with pytest.raises(ValueError):
            validate_source(dict(release["payload"]["artifact"], url=url), config["policy"])
    archive = tmp_path / "malicious.tar"
    with tarfile.open(str(archive), "w") as output:
        item = tarfile.TarInfo("../escape")
        item.size = 1
        output.addfile(item, io.BytesIO(b"x"))
    with pytest.raises(ValueError):
        extract(archive, tmp_path / "extracted", {})
    assert not (tmp_path / "escape").exists()


def test_immutable_id_rejects_different_artifact(config, keys, https, tmp_path):
    configured(config, https)
    controller = Controller(config)
    engine = DeploymentEngine(config, controller)
    first = release_tree(tmp_path, keys, https, "v1")
    engine.submit(command(keys, "deployment.apply", {"release": first}))
    assert finish(engine, "request-1")["status"] == "succeeded"
    changed = copy.deepcopy(first["payload"])
    changed["artifact"]["sha256"] = "c" * 64
    envelope = sign(changed, keys["publisher"][0], "publisher")
    engine.submit(command(keys, "deployment.apply", {"release": envelope}, "immutable"))
    assert finish(engine, "immutable")["status"] == "failed"
    assert controller.current == "v1" and controller.stops == 1


def test_schema_change_without_migration_rejected(config, keys, https, tmp_path):
    configured(config, https)
    controller = Controller(config)
    engine = DeploymentEngine(config, controller)
    first = release_tree(tmp_path, keys, https, "v1")
    engine.submit(command(keys, "deployment.apply", {"release": first}))
    assert finish(engine, "request-1")["status"] == "succeeded"
    second = release_tree(tmp_path, keys, https, "v2", state_schema="2")
    engine.submit(command(keys, "deployment.apply", {"release": second}, "new-schema"))
    assert finish(engine, "new-schema")["status"] == "failed"
    assert controller.stops == 1 and controller.current == "v1"


def test_reversible_schema_migration_requires_explicit_restore_on_rollback(config, keys, https, tmp_path):
    configured(config, https)
    engine = DeploymentEngine(config, Controller(config))
    first = release_tree(tmp_path, keys, https, "v1")
    engine.submit(command(keys, "deployment.apply", {"release": first}))
    assert finish(engine, "request-1")["status"] == "succeeded"
    state = Path(config["root"]) / "data/state/value"
    state.write_text("before-migration")
    second = release_tree(tmp_path, keys, https, "v2", state_schema="2", migration={
        "from_schema": "1", "strategy": "snapshot", "command": ["bin/host"], "timeout_s": 5})
    engine.submit(command(keys, "deployment.apply", {"release": second}, "migrate"))
    assert finish(engine, "migrate")["status"] == "succeeded"
    assert (state.parent / "migrated").exists()
    rollback = command(keys, "deployment.rollback", {"release_id": "v1", "restore_state": False}, "rollback-no-restore")
    with pytest.raises(ValueError, match="snapshot restoration"):
        engine.submit(approval(keys, rollback))
    rollback = command(keys, "deployment.rollback", {"release_id": "v1", "restore_state": True}, "rollback-restore")
    engine.submit(approval(keys, rollback, "restore-grant"))
    assert finish(engine, "rollback-restore")["status"] == "succeeded"
    assert state.read_text() == "before-migration" and not (state.parent / "migrated").exists()


def test_cancel_during_start_rolls_back_and_blocks_concurrent_deploy(config, keys, https, tmp_path):
    import threading
    configured(config, https)
    controller = Controller(config)
    engine = DeploymentEngine(config, controller)
    first = release_tree(tmp_path, keys, https, "v1")
    engine.submit(command(keys, "deployment.apply", {"release": first}))
    assert finish(engine, "request-1")["status"] == "succeeded"
    entered, resume = threading.Event(), threading.Event()
    original = controller.start
    def waiting_start(root, lock):
        if lock["release_id"] == "v2":
            entered.set()
            assert resume.wait(10)
        original(root, lock)
    controller.start = waiting_start
    second = release_tree(tmp_path, keys, https, "v2")
    engine.submit(command(keys, "deployment.apply", {"release": second}, "cancel-me"))
    assert entered.wait(10)
    try:
        with pytest.raises(ValueError, match="in progress"):
            engine.submit(command(keys, "deployment.apply", {"release": second}, "parallel"))
        engine.submit(command(keys, "deployment.cancel", {"request_id": "cancel-me"}, "cancel"))
    finally:
        resume.set()
    result = finish(engine, "cancel-me")
    assert result["status"] == "cancelled" and engine.active()["release_id"] == controller.current == "v1"


def test_approval_expiration_and_device_recovery_does_not_download(config, keys, https, tmp_path):
    configured(config, https)
    config["policy"]["auto_deploy"] = []
    engine = DeploymentEngine(config, Controller(config))
    release = release_tree(tmp_path, keys, https, "v1")
    request = approval(keys, command(keys, "deployment.apply", {"release": release}), ttl=-1)
    with pytest.raises(ValueError, match="expired"):
        engine.submit(request)
    approval(keys, request, grant_id="valid")
    engine.submit(request)
    assert finish(engine, "request-1")["status"] == "succeeded"
    config["policy"]["artifact_origins"] = []
    recovered = DeploymentEngine(config, Controller(config))
    recovered.recover()
    assert recovered.controller.current == "v1", "installed release recovery must not require its former origin"


def test_partial_download_and_symlink_archive_rejection(config, keys, https, tmp_path):
    configured(config, https)
    release = release_tree(tmp_path, keys, https, "v1")
    artifact = release["payload"]["artifact"]
    cache = tmp_path / "cache"
    cache.mkdir()
    (cache / (artifact["sha256"] + ".partial")).write_bytes((https["root"] / "v1.tar.gz").read_bytes()[:100])
    downloaded = download(artifact, cache, config["policy"])
    assert downloaded.stat().st_size == artifact["size"]
    archive = tmp_path / "symlink.tar"
    with tarfile.open(str(archive), "w") as output:
        member = tarfile.TarInfo("link")
        member.type = tarfile.SYMTYPE
        member.linkname = "/etc"
        output.addfile(member)
    with pytest.raises(ValueError, match="unsupported"):
        extract(archive, tmp_path / "symlink-extracted", {})


def test_auto_policy_profile_and_publisher_scope(config, keys, https, tmp_path):
    configured(config, https)
    config["trust"]["other-publisher"] = dict(config["trust"]["publisher"])
    engine = DeploymentEngine(config, Controller(config))
    release = release_tree(tmp_path, keys, https, "v1")
    wrong_profile = sign(dict(release["payload"], profile_id="other"), keys["publisher"][0], "publisher")
    wrong_publisher = sign(release["payload"], keys["publisher"][0], "other-publisher")
    for index, unauthorized in enumerate((wrong_profile, wrong_publisher)):
        with pytest.raises(ValueError):
            engine.submit(command(keys, "deployment.apply", {"release": unauthorized}, "scope-%d" % index))
    assert not list((Path(config["root"]) / "tasks").glob("*.json"))
