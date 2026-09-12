import hashlib
import http.server
import io
import json
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tarfile
import tempfile
import threading
import time

installer, executable, library, build_dir, example = sys.argv[1:]


def cli(*args, success=True):
    completed = subprocess.run([sys.executable, installer, *map(str, args)],
                               capture_output=True, text=True, timeout=40)
    result = json.loads(completed.stdout)
    assert (completed.returncode == 0) == success, (result, completed.stderr)
    return result


def request(path, **body):
    result = cli("request", "--socket", path, json.dumps(body))
    assert result["ok"], result
    return result


with tempfile.TemporaryDirectory(prefix="uran-install-test-") as temporary:
    root = Path(temporary)
    sdk = root / "sdk"
    subprocess.run(["cmake", "--install", build_dir, "--prefix", str(sdk)],
                   check=True, capture_output=True, text=True, timeout=30)
    store = root / "store"
    source = root / "prebuilt"
    prefix = source / "prefix"
    prefix.mkdir(parents=True)
    shutil.copyfile(library, prefix / "loopback.so")
    metadata = {"schema_version": 1, "package": "test_loopback", "version": "1.0.0", "api_version": "2.0"}
    (source / "uran-package.json").write_text(json.dumps(metadata | {"build": {"kind": "prebuilt"}}))
    (prefix / "plugin.json").write_text(json.dumps(metadata | {"plugins": [{
        "class_type": "uran_core_example_plugins/LoopbackProtocolPlugin", "library": "loopback.so"}]}))
    archive = root / "plugin.tar.gz"
    cli("pack", source, "--output", archive)
    with tarfile.open(root / "bad.tar", "w") as bad:
        entry = tarfile.TarInfo("../escape")
        entry.size = 3
        bad.addfile(entry, io.BytesIO(b"bad"))
    assert not cli("install", root / "bad.tar", "--store", store, success=False)["ok"]
    assert not (root / "escape").exists()
    assert not (store / "index.json").exists()
    assert not cli("install", archive, "--store", store, "--sha256", "0" * 64, success=False)["ok"]
    assert not (store / "index.json").exists()

    sock = root / "runtime.sock"
    # Use the installed binary to exercise relocatable SDK/runtime installation.
    runtime = subprocess.Popen([str(sdk / "bin/uran_runtime"), "--control-socket", str(sock)],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        deadline = time.monotonic() + 5
        while not sock.exists() and time.monotonic() < deadline:
            if runtime.poll() is not None:
                raise AssertionError(runtime.communicate())
            time.sleep(0.01)
        assert sock.exists()
        applied = cli("install", archive, "--store", store, "--runtime-socket", sock,
                      "--instance-id", "downloaded", "--component-id", "communication")
        assert applied["installed"] and applied["applied"]["ok"], applied
        assert request(sock, op="status")["result"]["protocol_instance_count"] == 1
        request(sock, op="publish", message_type="echo", payload={"downloaded": True})
        request(sock, op="unload", instance_id="downloaded")
        assert request(sock, op="status")["result"]["protocol_instance_count"] == 0

        # A separately built source plugin uses only the installed public SDK.
        source_archive = root / "source.tar.gz"
        cli("pack", example, "--output", source_archive)
        class Repository(http.server.SimpleHTTPRequestHandler):
            def __init__(self, *args, **kwargs):
                super().__init__(*args, directory=str(root), **kwargs)
            def log_message(self, *args):
                pass
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Repository)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            url = f"http://127.0.0.1:{server.server_port}/source.tar.gz"
            digest = hashlib.sha256(source_archive.read_bytes()).hexdigest()
            installed = cli("install", url, "--sha256", digest, "--store", store,
                            "--sdk-prefix", sdk, "--runtime-socket", sock, "--instance-id", "extension")
            assert installed["applied"]["ok"], installed
        finally:
            server.shutdown()
            server.server_close()
            thread.join()
        request(sock, op="dispatch", message_type="example.echo", payload={"value": 42},
                target={"device_id": "robot_001", "capability_id": "example.echo"})
        snapshot = request(sock, op="snapshot")["result"]
        assert snapshot["plugin.extension.last_message"] == {"value": 42}

        # A source build failure must preserve both the catalog and running plugin.
        previous_index = (store / "index.json").read_bytes()
        broken = root / "broken"
        shutil.copytree(example, broken)
        package = json.loads((broken / "uran-package.json").read_text())
        package["version"] = "broken"
        (broken / "uran-package.json").write_text(json.dumps(package))
        (broken / "CMakeLists.txt").write_text('cmake_minimum_required(VERSION 3.16)\nmessage(FATAL_ERROR "test build failure")\n')
        cli("pack", broken, "--output", root / "broken.tar.gz")
        cli("install", root / "broken.tar.gz", "--store", store, "--sdk-prefix", sdk, success=False)
        assert (store / "index.json").read_bytes() == previous_index
        assert not (store / "packages/example_extension/broken").exists()
        assert request(sock, op="snapshot")["result"]["plugin.extension.last_message"] == {"value": 42}

        # Transport failure during application still reports the installed artifact.
        detached = cli("install", source_archive, "--store", root / "detached", "--sdk-prefix", sdk,
                       "--runtime-socket", root / "missing.sock", "--instance-id", "extension", success=False)
        assert detached["installed"] and not detached["applied"]["ok"]
        assert Path(detached["manifest"]).exists()
        request(sock, op="shutdown")
        runtime.communicate(timeout=5)
        assert runtime.returncode == 0
    finally:
        if runtime.poll() is None:
            runtime.terminate()
            runtime.communicate(timeout=5)

    # A fresh process discovers installed versions from the store index.
    restarted = subprocess.run([str(sdk / "bin/uran_runtime"), "--plugin-index", str(store / "index.json")],
                               input='{"op":"classes"}\n', text=True, capture_output=True, timeout=10, check=True)
    assert "example/Extension" in json.loads(restarted.stdout)["result"]
print("Plugin installation: archive validation, download, SDK compilation, live application and restart passed")
