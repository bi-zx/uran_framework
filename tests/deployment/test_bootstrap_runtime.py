import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

import pytest

from uran_deploy.artifacts import extract
from uran_deploy.bootstrap import install
from uran_deploy.common import atomic_json, read_json, rpc
from uran_deploy.packaging import build_bootstrap, build_release, copy_runtime_sdk

from test_transactions import command


def wait_for(function, timeout=25):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            result = function()
            if result:
                return result
        except (OSError, ValueError, KeyError) as error:
            last = error
        time.sleep(0.1)
    raise AssertionError("condition not reached: %s" % last)


def start_supervisor(prefix, config_path, log):
    # A private bundle, no source checkout or global PYTHONPATH on the device.
    environment = {key: value for key, value in os.environ.items() if not key.startswith("PYTHON")}
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    return subprocess.Popen([sys.executable, str(prefix / "bin/uran-deploy"), "supervise",
        "--config", str(config_path)], stdout=log, stderr=log, env=environment)


def native_release(tmp_path, sdk, keys, https, name, bad_check=False, slow_check=False, python_site=None):
    source = tmp_path / (name + "-source")
    copy_runtime_sdk(sdk, source)
    atomic_json(source / "runtime.json", {"plugins": []})
    checks = []
    post_activate = []
    if python_site:
        import shutil
        shutil.copytree(str(python_site / "uran_worker_sdk"), str(source / "python/uran_worker_sdk"),
                        ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
        worker = source / "bin/business-worker"
        worker.write_text('import pathlib,sys,os\n'
            'sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/"python"))\n'
            'from uran_worker_sdk import Worker,WorkerServer\n'
            'marker=pathlib.Path(os.environ["URAN_DATA_DIR"])/"business-active"\n'
            'class Business(Worker):\n'
            '    plugin_id="business"\n'
            '    def activate(self): marker.write_text("active")\n'
            '    def deactivate(self):\n'
            '        if marker.exists(): marker.unlink()\n'
            'WorkerServer(Business()).serve()\n')
        runtime = {"plugins": [{"instance_id": "business", "component_id": "test", "device_id": "${device_id}",
            "class_type": "uran_worker_adapter/WorkerProcessPlugin", "config": {
                "command": [sys.executable, "${release}/bin/business-worker"],
                "message_types": ["test.business"], "worker_config": {}}}]}
        atomic_json(source / "runtime.json", runtime)
        for expected in ("configured", "active"):
            check = source / ("check-" + expected)
            check.write_text('#!/usr/bin/env python3\nimport json,os,socket,pathlib\n'
                's=socket.socket(socket.AF_UNIX); s.connect(os.environ["URAN_CORE_SOCKET"])\n'
                's.sendall(b\'{"op":"status"}\\n\')\n'
                'reply=json.loads(s.makefile("rb").readline())\n'
                'state=next(i["state"] for i in reply["result"]["plugins"] if i["instance_id"]=="business")\n'
                'assert state==%r,state\n' % expected +
                'assert (pathlib.Path(os.environ["URAN_DATA_DIR"])/"business-active").exists()==%r\n' % (expected == "active"))
            check.chmod(0o755)
            (checks if expected == "configured" else post_activate).append(
                {"command": [check.name], "timeout_s": 5})
    if bad_check or slow_check:
        path = source / "check"
        if slow_check:
            path.write_text('#!/bin/sh\nsleep 25\n')
        else:
            path.write_text('#!/bin/sh\nprintf broken > "$URAN_DATA_DIR/failed-check"\nexit 1\n')
        path.chmod(0o755)
        checks = [{"command": ["check"], "timeout_s": 30}]
    spec = {"release_id": name, "profile_id": "test", "profile_version": name,
            "api_version": "2.1", "state_schema": "1", "platform": {"os": "Linux", "arch": __import__("platform").machine()},
            "host": "bin/uran_runtime", "runtime_config": "runtime.json", "checks": checks, "post_activate": post_activate,
            "packages": [{"package": "framework", "version": "2.1.0", "origin": "test:installed-sdk", "sha256": "a" * 64}]}
    return build_release(source, spec, https["root"] / (name + ".tar.gz"),
        https["url"] + "/" + name + ".tar.gz", keys["publisher"][0], "publisher")


def setup_bootstrap(config, keys, https, tmp_path, sdk, python_site):
    config["policy"].update(artifact_origins=[https["url"]], artifact_ca_file=https["ca"])
    archive = tmp_path / "bootstrap.tar.gz"
    build_bootstrap(sdk, python_site, archive, keys["publisher"][0], "publisher",
                    https["url"] + "/seed.tar.gz", "test:installed-sdk")
    subprocess.run(["openssl", "dgst", "-sha256", "-verify", str(keys["publisher"][1]),
                    "-signature", str(archive) + ".sig", str(archive)], check=True, stdout=subprocess.PIPE)
    bundle = tmp_path / "bundle"
    extract(archive, bundle, {})
    config_path = tmp_path / "device-config.json"
    atomic_json(config_path, config)
    prefix, root = Path(config["bootstrap_prefix"]), Path(config["root"])
    installed_config = Path(install(bundle, config_path, root, prefix, keys["publisher"][1]))
    return prefix, root, installed_config


def test_bootstrap_real_core_upgrade_failed_health_and_supervisor_restart(config, keys, https, tmp_path, sdk, python_site):
    prefix, root, installed_config = setup_bootstrap(config, keys, https, tmp_path, sdk, python_site)
    log = open(str(tmp_path / "supervisor.log"), "ab", buffering=0)
    supervisor = start_supervisor(prefix, installed_config, log)
    try:
        wait_for(lambda: rpc(root / "run/core.sock", {"op": "status"}).get("ok"))
        status = rpc(root / "run/core.sock", {"op": "status"})
        assert any(item["instance_id"] == "uran_management" for item in status["result"]["plugins"])
        first = native_release(tmp_path, sdk, keys, https, "v1", python_site=python_site)
        # Drive the actual Core -> process worker -> supervisor path.
        signed = command(keys, "deployment.apply", {"release": first})
        result = rpc(root / "run/core.sock", {"op": "dispatch", "message_type": "uran.management.command.v1",
            "correlation_id": "request-1", "target": {"instance_id": "uran_management"}, "payload": signed})
        assert result["ok"]
        wait_for(lambda: read_json(root / "tasks/request-1.json")["status"] == "succeeded")
        assert read_json(root / "active.json")["release_id"] == "v1"
        second = native_release(tmp_path, sdk, keys, https, "v2", bad_check=True)
        assert rpc(root / "run/supervisor.sock", command(keys, "deployment.apply", {"release": second}, "bad"))["ok"]
        wait_for(lambda: read_json(root / "tasks/bad.json")["status"] == "failed")
        assert read_json(root / "active.json")["release_id"] == "v1"
        assert not (root / "data/state/failed-check").exists()
        assert rpc(root / "run/core.sock", {"op": "status"})["ok"]
        # Interrupt a running new host during checks. Recovery must restore v1,
        # stop recorded processes and retain a terminal result for the caller.
        third = native_release(tmp_path, sdk, keys, https, "v3", slow_check=True)
        assert rpc(root / "run/supervisor.sock", command(keys, "deployment.apply", {"release": third}, "crash"))["ok"]
        wait_for(lambda: read_json(root / "tasks/crash.json")["phase"] == "checking")
        wait_for(lambda: any(item["name"] == "checks" for item in read_json(root / "run/processes.json")))
        supervisor.kill()
        supervisor.wait(timeout=5)
        supervisor = start_supervisor(prefix, installed_config, log)
        wait_for(lambda: read_json(root / "tasks/crash.json")["status"] == "failed")
        wait_for(lambda: rpc(root / "run/core.sock", {"op": "status"}).get("ok"))
        assert read_json(root / "active.json")["release_id"] == "v1"
        assert os.readlink(str(root / "current")) == "releases/v1"
        # A committed release can become unreadable later. Keep cloud management
        # available through the empty bootstrap Core, with separate state.
        supervisor.terminate()
        supervisor.wait(timeout=20)
        runtime_file = root / "releases/v1/runtime.json"
        runtime_file.chmod(0o644)
        runtime_file.write_text('{"corrupted":true}')
        (root / "data/state/preserved").write_text("product-state")
        supervisor = start_supervisor(prefix, installed_config, log)
        wait_for(lambda: read_json(root / "run/recovery.json")["mode"] == "management_recovery")
        wait_for(lambda: rpc(root / "run/core.sock", {"op": "status"}).get("ok"))
        status = rpc(root / "run/core.sock", {"op": "status"})
        assert {item["instance_id"] for item in status["result"]["plugins"]} == {"uran_management"}
        assert (root / "data/state/preserved").read_text() == "product-state"
        assert read_json(root / "active.json")["release_id"] == "v1"
        repair = native_release(tmp_path, sdk, keys, https, "v4")
        signed = command(keys, "deployment.apply", {"release": repair}, "repair")
        assert rpc(root / "run/core.sock", {"op": "dispatch", "message_type": "uran.management.command.v1",
            "correlation_id": "repair", "target": {"instance_id": "uran_management"}, "payload": signed})["ok"]
        wait_for(lambda: read_json(root / "tasks/repair.json")["status"] == "succeeded")
        assert read_json(root / "run/recovery.json")["mode"] == "normal"
        assert read_json(root / "active.json")["release_id"] == "v4"
    finally:
        supervisor.terminate()
        supervisor.wait(timeout=20)
        log.close()
    assert supervisor.returncode == 0, (tmp_path / "supervisor.log").read_text()
    with pytest.raises(ValueError, match="existing deployments"):
        install(tmp_path / "bundle", installed_config, root, prefix, keys["publisher"][1])
    assert read_json(root / "active.json")["release_id"] == "v4"


def test_tls_mqtt_core_deployment_and_legacy_tunnel_denial(config, keys, https, tmp_path, sdk, python_site):
    import shutil
    import socket
    assert shutil.which("mosquitto"), "integration testing requires a local mosquitto executable"
    with socket.socket() as socket_probe:
        socket_probe.bind(("127.0.0.1", 0))
        port = socket_probe.getsockname()[1]
    broker_config = tmp_path / "mosquitto.conf"
    broker_config.write_text("listener %d 127.0.0.1\nallow_anonymous true\ncertfile %s\nkeyfile %s\n" %
                            (port, https["ca"], https["key"]))
    log = open(str(tmp_path / "mqtt-integration.log"), "ab", buffering=0)
    broker = subprocess.Popen(["mosquitto", "-c", str(broker_config)], stdout=log, stderr=log)
    supervisor = None
    cloud = None
    try:
        def broker_ready():
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return True
        wait_for(broker_ready, timeout=5)
        config["cloud"] = {"transport_enabled": True, "device_id": "test_device", "tenant_id": "test",
            "auth": {"username": "test_device", "token": "test_device-secret"},
            "mqtt": {"broker_host": "localhost", "broker_port": port,
                     "tls": {"enabled": True, "ca_file": https["ca"]}},
            "registration": {"require_response": False, "legacy_online_on_timeout": True},
            "register_timeout_s": 0.2, "heartbeat_interval_s": 0.3, "report_on_change": False}
        prefix, root, installed_config = setup_bootstrap(config, keys, https, tmp_path, sdk, python_site)
        supervisor = start_supervisor(prefix, installed_config, log)
        wait_for(lambda: rpc(root / "run/core.sock", {"op": "status"}).get("ok"))
        sys.path.insert(0, str(python_site))
        import paho.mqtt.client as mqtt
        cloud = mqtt.Client(client_id="deployment-integration-cloud")
        cloud.tls_set(ca_certs=https["ca"])
        messages = []
        subscribed = []
        cloud.on_message = lambda _client, _data, message: messages.append(json.loads(message.payload))
        cloud.on_subscribe = lambda *_args: subscribed.append(True)
        cloud.connect("localhost", port, 30)
        cloud.subscribe("/oivs/test/test_device/up")
        cloud.loop_start()
        wait_for(lambda: subscribed)
        release = native_release(tmp_path, sdk, keys, https, "mqtt-v1")
        envelope = command(keys, "deployment.apply", {"release": release}, "mqtt-deploy")
        packet = {"msg_type": "management_cmd", "device_id": "test_device", "envelope": envelope}
        cloud.publish("/oivs/test/test_device/down", json.dumps(packet), qos=1).wait_for_publish()
        wait_for(lambda: read_json(root / "tasks/mqtt-deploy.json")["status"] == "succeeded")
        wait_for(lambda: any(item.get("data_type") == "uran.management.state.v1" and
            item.get("payload", {}).get("request_id") == "mqtt-deploy" and
            item["payload"].get("status") == "succeeded" for item in messages))
        cloud.publish("/oivs/test/test_device/down", json.dumps({"msg_type": "frpc_ctrl", "device_id": "test_device", "action": "start"}), qos=1).wait_for_publish()
        wait_for(lambda: any(item.get("reason") == "frpc_requires_signed_user_grant" for item in messages))
        assert not (root / "run/maintenance.json").exists()
        assert read_json(root / "active.json")["release_id"] == "mqtt-v1"
    finally:
        if cloud:
            cloud.disconnect()
            cloud.loop_stop()
        if supervisor:
            supervisor.terminate()
            supervisor.wait(timeout=20)
        broker.terminate()
        broker.wait(timeout=5)
        log.close()
