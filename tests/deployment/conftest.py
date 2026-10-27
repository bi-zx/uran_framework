import functools
import json
from http.server import SimpleHTTPRequestHandler, HTTPServer
import os
from pathlib import Path
import socketserver
import ssl
import subprocess
import sys
import threading

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python/uran_deploy"))

from uran_deploy.common import atomic_json


@pytest.fixture(scope="session")
def keys(tmp_path_factory):
    root = tmp_path_factory.mktemp("deployment-keys")
    result = {}
    for role in ("agent", "user", "publisher"):
        private, public = root / (role + ".key"), root / (role + ".pub")
        subprocess.run(["openssl", "genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:2048", "-out", str(private)],
                       check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        subprocess.run(["openssl", "pkey", "-in", str(private), "-pubout", "-out", str(public)],
                       check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        result[role] = (private, public)
    return result


@pytest.fixture
def config(tmp_path, keys):
    return {"schema_version": "1.0", "device_id": "test_device", "root": str(tmp_path / "device"),
            "bootstrap_prefix": str(tmp_path / "bootstrap"), "python": sys.executable,
            "trust": {role: {"role": role, "subject": role, "public_key": str(paths[1])}
                      for role, paths in keys.items()},
            "policy": {"artifact_origins": ["https://localhost"], "reserve_bytes": 0,
                "auto_deploy": [{"profile_id": "test", "agents": ["agent"],
                                 "publishers": ["publisher"], "actions": ["deployment.apply"]}]},
            "cloud": {"transport_enabled": False}, "maintenance": {"enabled": False}}


@pytest.fixture
def https(tmp_path):
    root = tmp_path / "artifacts"
    root.mkdir()
    key, certificate = tmp_path / "tls.key", tmp_path / "tls.crt"
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
        "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
        "-keyout", str(key), "-out", str(certificate)], check=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    class Handler(SimpleHTTPRequestHandler):
        def log_message(self, *args):
            pass
        def do_POST(self):
            if self.path == "/redirect-enroll":
                self.send_response(307)
                self.send_header("Location", "/enroll")
                self.end_headers()
                return
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            if (self.path != "/enroll" or body.get("registration_code") != "one-time" or
                    body.get("device_id") != "test_device" or getattr(self.server, "enrollment_used", False)):
                self.send_error(403)
                return
            self.server.enrollment_used = True
            result = json.dumps({"device_id": body["device_id"],
                "cloud": {"auth": {"username": "test_device", "token": "independent-device-token"},
                          "mqtt": {"broker_host": "mqtt.example.test", "tls": {"enabled": True}}}}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(result)))
            self.end_headers()
            self.wfile.write(result)
    class Server(socketserver.ThreadingMixIn, HTTPServer):
        daemon_threads = True
    server = Server(("127.0.0.1", 0), functools.partial(Handler, directory=str(root)))
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(str(certificate), str(key))
    server.socket = context.wrap_socket(server.socket, server_side=True)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    yield {"root": root, "url": "https://localhost:%d" % server.server_port,
           "ca": str(certificate), "key": str(key)}
    server.shutdown()
    server.server_close()
    thread.join(timeout=3)


@pytest.fixture(scope="session")
def sdk():
    path = Path(os.environ.get("URAN_DEPLOY_TEST_SDK", "/tmp/uran-deployment-work/sdk"))
    assert (path / "bin/uran_runtime").is_file(), "set URAN_DEPLOY_TEST_SDK to an installed native SDK"
    return path


@pytest.fixture(scope="session")
def python_site():
    path = Path(os.environ.get("URAN_DEPLOY_TEST_PYTHON_SITE", "/tmp/uran-deployment-work/site"))
    assert (path / "uran_mqtt_plugin").is_dir(), "set URAN_DEPLOY_TEST_PYTHON_SITE to installed worker dependencies"
    return path
