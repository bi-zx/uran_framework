"""One-time local enrollment/installation. Not exposed by the management API."""
import json
import os
from pathlib import Path
import pwd
import re
import shutil
import ssl
import subprocess
import sys
import tempfile
import urllib.request

from .artifacts import extract, freeze, platform_check, validate_release
from .common import atomic_json, file_hash, identifier, read_json, sync_dir
from .packaging import inventory
from .security import Authority


def validate_config(config):
    if config.get("schema_version") != "1.0":
        raise ValueError("unsupported device configuration")
    identifier(config["device_id"])
    if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", config["device_id"]):
        raise ValueError("device ID must match the Core identity syntax")
    if not config.get("trust"):
        raise ValueError("device trust must be provisioned locally")
    roles_by_key = {}
    for key_id, key in config["trust"].items():
        identifier(key_id)
        if set(key) - {"role", "subject", "public_key", "disabled"} or key.get("role") not in {"user", "agent", "publisher"}:
            raise ValueError("invalid device trust entry")
        identifier(key["subject"])
        if not Path(key["public_key"]).is_file():
            raise ValueError("trusted public key does not exist")
        # This implementation deliberately has one portable signature algorithm.
        check = subprocess.run(["openssl", "rsa", "-pubin", "-in", key["public_key"], "-text", "-noout"],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
        if check.returncode or b"(2048 bit)" not in check.stdout and b"(3072 bit)" not in check.stdout and b"(4096 bit)" not in check.stdout:
            raise ValueError("trusted keys must be RSA public keys of 2048, 3072 or 4096 bits")
        if roles_by_key.get(check.stdout, key["role"]) != key["role"]:
            raise ValueError("agent, user and publisher roles require independent keys")
        roles_by_key[check.stdout] = key["role"]
    cloud = config.get("cloud", {})
    if cloud.get("transport_enabled", True):
        mqtt = cloud.get("mqtt", {})
        if not mqtt.get("tls", {}).get("enabled", False):
            raise ValueError("bootstrap cloud MQTT requires TLS")
        if not cloud.get("auth", {}).get("token"):
            raise ValueError("device MQTT credential must be provisioned")
        if not mqtt.get("broker_host"):
            raise ValueError("cloud broker is required")
    policy = config.get("policy", {})
    from .artifacts import origin
    for url in policy.get("artifact_origins", []):
        origin(url)
    for rule in policy.get("auto_deploy", []):
        if set(rule) != {"profile_id", "publishers", "agents", "actions"}:
            raise ValueError("auto deployment rules must explicitly declare their scope")
        identifier(rule["profile_id"])
        if not set(rule["actions"]) <= {"deployment.apply", "deployment.rollback"}:
            raise ValueError("auto deployment rules cannot grant remote maintenance")
        for key_id in rule["publishers"]:
            if config["trust"].get(key_id, {}).get("role") != "publisher":
                raise ValueError("auto deployment references an unknown publisher")
    if config.get("maintenance", {}).get("enabled", False):
        if not config["maintenance"].get("provider_command"):
            raise ValueError("maintenance requires a locally provisioned provider")


def enroll(endpoint, registration_code, device_id, ca_file=None):
    from .artifacts import origin
    origin(endpoint)
    request = urllib.request.Request(endpoint,
        data=json.dumps({"schema_version": "1.0", "device_id": identifier(device_id),
                         "registration_code": registration_code}).encode(),
        headers={"Content-Type": "application/json"}, method="POST")
    # Registration responses are credentials, never persisted in logs. Redirects
    # are forbidden so an enrollment code cannot move to a different endpoint.
    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, *args, **kwargs):
            raise ValueError("enrollment redirects are forbidden")
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect(),
        urllib.request.HTTPSHandler(context=ssl.create_default_context(cafile=ca_file)))
    try:
        with opener.open(request, timeout=15) as response:
            from .common import loads
            data = response.read(65537)
            if len(data) > 65536:
                raise ValueError("enrollment response exceeds limit")
            result = loads(data)
    except OSError:
        raise ValueError("device enrollment failed")
    if set(result) != {"device_id", "cloud"} or result["device_id"] != device_id:
        raise ValueError("invalid device enrollment response")
    return result["cloud"]


def install(bundle, config_path, root, prefix, bootstrap_key, python=sys.executable,
            service_user=None, unit_dir="/etc/systemd/system"):
    bundle, root, prefix = Path(bundle).resolve(), Path(root).resolve(), Path(prefix).resolve()
    if root == prefix or root in prefix.parents or prefix in root.parents:
        raise ValueError("bootstrap code and mutable device data must be separate directories")
    if prefix.exists() or root.exists() and any(root.iterdir()):
        raise ValueError("installation destinations must be new or empty; existing deployments are preserved")
    envelope = read_json(bundle / "bootstrap.json")
    trust = {envelope["key_id"]: {"public_key": str(Path(bootstrap_key).resolve()),
                                "role": "publisher", "subject": "bootstrap"}}
    manifest, _ = Authority({"trust": trust}, root).verify(envelope, {"publisher"}, "bootstrap")
    if set(manifest) != {"schema_version", "kind", "version", "platform", "files"}:
        raise ValueError("invalid bootstrap manifest")
    actual = [entry for entry in inventory(bundle) if entry["path"] != "bootstrap.json"]
    if actual != manifest["files"]:
        raise ValueError("bootstrap inventory verification failed")
    # Bootstrap has the same platform/API requirements as its seed release.
    platform_check({"platform": manifest["platform"], "api_version": "2.1", "native_abi": 3})
    config = read_json(config_path)
    config.update(root=str(root), bootstrap_prefix=str(prefix), python=str(Path(python).resolve()))
    for key in config.get("trust", {}).values():
        path = Path(key["public_key"])
        if not path.is_absolute():
            key["public_key"] = str((Path(config_path).resolve().parent / path).resolve())
    validate_config(config)
    account = pwd.getpwnam(service_user) if service_user else None
    if account and (os.geteuid() != 0 or account.pw_uid == 0):
        raise ValueError("system installation requires root and a non-root service account")
    protected_config = (prefix.parent / ("device-" + config["device_id"])) if account else (root / "config")
    if protected_config != root / "config" and protected_config.exists():
        raise ValueError("device configuration destination already exists")
    if account:
        prefix.parent.mkdir(parents=True, exist_ok=True)
        for ancestor in [prefix.parent] + list(prefix.parent.parents):
            metadata = ancestor.stat()
            if metadata.st_uid != 0 or metadata.st_mode & 0o022:
                raise ValueError("production bootstrap/config ancestors must be root-owned and not group/world writable")
        service_unit(dict(config, device_config=str(protected_config / "device.json")), service_user)
        if (Path(unit_dir) / "uran-supervisor.service").exists():
            raise ValueError("existing URAN supervisor service is preserved")
    # Check target Python and shared-library dependencies before touching data.
    target_python = subprocess.run([python, "-c", "import sys; assert sys.version_info >= (3,6); "
        "print('%d.%d' % sys.version_info[:2])"], check=True, stdout=subprocess.PIPE).stdout.decode("ascii").strip()
    if manifest["platform"].get("python") and target_python != manifest["platform"]["python"]:
        raise ValueError("bootstrap Python version does not match the selected service interpreter")
    subprocess.run([str(bundle / "sdk/bin/uran_runtime"), "--help"], check=True, stdout=subprocess.PIPE)
    subprocess.run([python, "-B", "-c", "import sys; sys.path.insert(0, sys.argv[1]); "
                    "import uran_worker_sdk, uran_mqtt_plugin.worker, uran_deploy.worker, paho.mqtt.client",
                    str(bundle / "python")], check=True)
    seed_envelope = read_json(bundle / "seed.tar.gz.release.json")
    release, _ = Authority(config, root).release(seed_envelope)
    archive = bundle / "seed.tar.gz"
    if archive.stat().st_size != release["artifact"]["size"] or file_hash(archive) != release["artifact"]["sha256"]:
        raise ValueError("bootstrap seed digest mismatch")
    with tempfile.TemporaryDirectory(prefix="uran-seed-check-") as temporary:
        staged = Path(temporary) / "seed"
        extract(archive, staged, config.get("policy", {}))
        validate_release(staged, release, config.get("policy", {}))
    root.mkdir(parents=True, exist_ok=True)
    root.chmod(0o700)
    prefix.parent.mkdir(parents=True, exist_ok=True)
    # The prefix must equal the verified inventory; bytecode is never part of it.
    shutil.copytree(str(bundle), str(prefix), ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    freeze(prefix)
    for name in ("releases", "receipts", "run", "data/state", "data/shared", "logs"):
        (root / name).mkdir(parents=True, exist_ok=True)
    (protected_config / "keys").mkdir(parents=True, exist_ok=True)
    for key_id, key in config["trust"].items():
        destination = protected_config / "keys" / (key_id + ".pem")
        shutil.copyfile(key["public_key"], str(destination))
        destination.chmod(0o400)
        key["public_key"] = str(destination)
    config["device_config"] = str(protected_config / "device.json")
    atomic_json(protected_config / "device.json", config)
    archive = prefix / "seed.tar.gz"
    destination = root / "releases" / release["release_id"]
    extract(archive, destination, config.get("policy", {}))
    validate_release(destination, release, config.get("policy", {}))
    freeze(destination)
    atomic_json(root / "receipts" / (release["release_id"] + ".json"),
        {"artifact_sha256": release["artifact"]["sha256"], "envelope": seed_envelope})
    atomic_json(root / "active.json", {"release_id": release["release_id"], "state_schema": release["state_schema"],
        "profile_id": release["profile_id"], "artifact_sha256": release["artifact"]["sha256"],
        "previous": None, "request_id": "bootstrap", "rollback_backup": None})
    os.symlink("releases/" + release["release_id"], str(root / "current"))
    sync_dir(root)
    if account:
        for path in [root] + list(root.rglob("*")):
            if not path.is_symlink():
                os.chown(str(path), account.pw_uid, account.pw_gid)
        # The service may read its config, but remotely installed code cannot
        # rewrite local trust or preauthorization rules.
        for path in [protected_config] + list(protected_config.rglob("*")):
            os.chown(str(path), 0, account.pw_gid)
            path.chmod(0o750 if path.is_dir() else 0o640)
        unit = service_unit(config, service_user)
        unit_path = Path(unit_dir) / "uran-supervisor.service"
        if unit_path.exists():
            raise ValueError("existing URAN supervisor service is preserved")
        unit_path.write_text(unit)
        subprocess.run(["systemctl", "daemon-reload"], check=True)
        subprocess.run(["systemctl", "enable", "--now", "uran-supervisor.service"], check=True)
    return str(protected_config / "device.json")


def service_unit(config, user):
    # Do not interpolate unescaped whitespace/directives into systemd syntax.
    for value in (config["root"], config["bootstrap_prefix"], config["python"],
                  config.get("device_config", config["root"] + "/config/device.json"), user):
        if any(char.isspace() or char in '\\"%$' for char in value):
            raise ValueError("system service paths/user must not contain whitespace or special characters")
    return """[Unit]
Description=URAN device deployment supervisor
Wants=network-online.target
After=network-online.target
StartLimitIntervalSec=120
StartLimitBurst=5

[Service]
Type=simple
User={user}
ExecStart={python} {prefix}/bin/uran-deploy supervise --config {device_config}
Restart=on-failure
RestartSec=5
TimeoutStopSec=60
KillMode=control-group
UMask=0077
NoNewPrivileges=yes
PrivateTmp=yes
ProtectSystem=strict
ProtectHome=yes
ReadWritePaths={root}

[Install]
WantedBy=multi-user.target
""".format(user=user, python=config["python"], prefix=config["bootstrap_prefix"], root=config["root"],
           device_config=config.get("device_config", config["root"] + "/config/device.json"))
