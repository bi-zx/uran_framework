#!/usr/bin/env python3
"""Install versioned native URAN plugins and optionally apply them to a running host.

Only Python's standard library is required. Download/install is platform tooling;
the C++ runtime knows neither repositories nor package build tools.
"""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import socket
import subprocess
import sys
import tarfile
import tempfile
import urllib.parse
import urllib.request

API_VERSION = "2.0"
NAME = re.compile(r"^[A-Za-z][A-Za-z0-9_.-]*$")


def contained(root, relative):
    relative = Path(relative)
    if relative.is_absolute():
        raise ValueError("manifest paths must be relative")
    path = (root / relative).resolve()
    path.relative_to(root.resolve())
    return path


def load_json(path):
    with open(path, encoding="utf-8") as stream:
        return json.load(stream)


def atomic_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, filename = tempfile.mkstemp(prefix=".index-", dir=path.parent)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            json.dump(value, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(filename, path)
    finally:
        if os.path.exists(filename):
            os.unlink(filename)


def fetch(uri, destination, expected):
    parsed = urllib.parse.urlparse(uri)
    if parsed.scheme in ("https", "http"):
        if not expected:
            raise ValueError("remote packages require --sha256")
        if parsed.scheme == "http" and parsed.hostname not in ("localhost", "127.0.0.1", "::1"):
            raise ValueError("remote repositories must use HTTPS")
        source = urllib.request.urlopen(uri, timeout=30)
    elif parsed.scheme == "file":
        source = open(urllib.request.url2pathname(parsed.path), "rb")
    elif not parsed.scheme:
        source = open(uri, "rb")
    else:
        raise ValueError("supported package sources: local file, file:// and HTTPS")
    digest = hashlib.sha256()
    with source, open(destination, "wb") as output:
        while True:
            chunk = source.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
            output.write(chunk)
    actual = digest.hexdigest()
    if expected and actual.lower() != expected.lower():
        raise ValueError("package SHA-256 mismatch")
    return actual


def extract(archive, root):
    seen = set()
    with tarfile.open(archive, "r:*") as bundle:
        total = 0
        for member in bundle:
            name = PurePosixPath(member.name)
            if name.is_absolute() or ".." in name.parts or member.issym() or member.islnk():
                raise ValueError("unsafe archive member: " + member.name)
            if not (member.isdir() or member.isfile()):
                raise ValueError("unsupported archive member: " + member.name)
            destination = contained(root, str(name))
            if member.isdir():
                destination.mkdir(parents=True, exist_ok=True)
                continue
            if destination in seen:
                raise ValueError("duplicate archive member: " + member.name)
            seen.add(destination)
            total += member.size
            if total > 2 * 1024**3 or len(seen) > 100000:
                raise ValueError("plugin archive exceeds extraction limits")
            destination.parent.mkdir(parents=True, exist_ok=True)
            with bundle.extractfile(member) as source, open(destination, "wb") as output:
                shutil.copyfileobj(source, output)
            destination.chmod(member.mode & 0o777)


def validate_package(manifest):
    if manifest.get("schema_version") != 1 or manifest.get("api_version") != API_VERSION:
        raise ValueError("unsupported package schema/API version")
    for key in ("package", "version"):
        value = manifest.get(key, "")
        # Versions may start with a digit; package IDs may not.
        pattern = NAME if key == "package" else re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]*$")
        if not isinstance(value, str) or not pattern.fullmatch(value):
            raise ValueError("invalid " + key)
    requirements = manifest.get("requires", {})
    for key, actual in (("os", platform.system()), ("arch", platform.machine())):
        accepted = requirements.get(key, [])
        if accepted and actual not in accepted:
            raise ValueError(f"unsupported {key}: {actual}; expected {accepted}")
    missing = [name for name in requirements.get("commands", []) if not shutil.which(name)]
    if missing:
        raise ValueError("missing build/runtime commands: " + ", ".join(missing))


def validate_plugin_manifest(path, prefix, package):
    plugin = load_json(path)
    if plugin.get("schema_version") != 1 or plugin.get("api_version") != API_VERSION:
        raise ValueError("incompatible plugin index")
    if plugin.get("package") != package["package"] or plugin.get("version") != package["version"]:
        raise ValueError("package and plugin manifest identities differ")
    entries = plugin.get("plugins")
    if not isinstance(entries, list) or not entries:
        raise ValueError("plugin index must contain at least one class")
    classes = set()
    for entry in entries:
        name = entry.get("class_type")
        if not isinstance(name, str) or not name or name in classes:
            raise ValueError("missing or duplicate plugin class")
        classes.add(name)
        relative = Path(entry["library"])
        if relative.is_absolute():
            raise ValueError("library paths must be relative to plugin.json")
        library = (path.parent / relative).resolve()
        library.relative_to(prefix.resolve())
        if not library.is_file():
            raise ValueError("plugin library does not exist: " + str(library))
    return plugin


def run_build(argv):
    # Shell-free execution; source plugins are compiled as ordinary developer code.
    result = subprocess.run(argv, stdout=sys.stderr, stderr=sys.stderr, check=False)
    if result.returncode:
        raise RuntimeError(f"build command failed ({result.returncode}): {argv[0]}")


def request(socket_path, payload):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(60)
        client.connect(socket_path)
        client.sendall((json.dumps(payload) + "\n").encode())
        reply = bytearray()
        while b"\n" not in reply:
            chunk = client.recv(65536)
            if not chunk:
                raise RuntimeError("runtime closed the management connection")
            reply.extend(chunk)
            if len(reply) > 4 * 1024 * 1024:
                raise ValueError("management reply too large")
    return json.loads(reply.split(b"\n", 1)[0])


def install(args):
    store = args.store.expanduser().resolve()
    store.mkdir(parents=True, exist_ok=True)
    with open(store / ".install.lock", "a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        with tempfile.TemporaryDirectory(prefix=".stage-", dir=store) as temporary:
            stage = Path(temporary)
            digest = fetch(args.uri, stage / "package.tar", args.sha256)
            source = stage / "source"
            source.mkdir()
            extract(stage / "package.tar", source)
            package = load_json(source / "uran-package.json")
            validate_package(package)
            destination = store / "packages" / package["package"] / package["version"]
            if destination.exists():
                raise ValueError("package version already installed; use a new immutable version")
            prefix = stage / "prefix"
            build = package.get("build", {"kind": "prebuilt"})
            if build["kind"] == "cmake":
                project = contained(source, build.get("source", "."))
                arguments = build.get("cmake_args", [])
                if not isinstance(arguments, list) or any(not isinstance(a, str) or not a.startswith("-D") for a in arguments):
                    raise ValueError("cmake_args must contain only -D definitions")
                if any(a.startswith(("-DCMAKE_INSTALL_PREFIX", "-DCMAKE_PREFIX_PATH")) for a in arguments):
                    raise ValueError("installation and SDK paths are controlled by the installer")
                configure = ["cmake", "-S", str(project), "-B", str(stage / "build"),
                             "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_INSTALL_PREFIX=" + str(prefix)]
                if args.sdk_prefix:
                    configure.append("-DCMAKE_PREFIX_PATH=" + str(args.sdk_prefix.resolve()))
                run_build(configure + arguments)
                run_build(["cmake", "--build", str(stage / "build"), "--parallel", str(args.jobs)])
                run_build(["cmake", "--install", str(stage / "build")])
            elif build["kind"] == "prebuilt":
                shutil.copytree(contained(source, build.get("prefix", "prefix")), prefix)
            else:
                raise ValueError("supported build kinds: prebuilt, cmake")
            relative_manifest = package.get("plugin_manifest", "plugin.json")
            manifest_path = contained(prefix, relative_manifest)
            plugin = validate_plugin_manifest(manifest_path, prefix, package)
            index_path = store / "index.json"
            index = load_json(index_path) if index_path.exists() else {"schema_version": 1, "packages": {}}
            if index.get("schema_version") != 1:
                raise ValueError("unsupported store index")
            destination.parent.mkdir(parents=True, exist_ok=True)
            os.replace(prefix, destination)
            installed_manifest = contained(destination, relative_manifest)
            index["packages"][package["package"]] = {
                "version": package["version"], "manifest": str(installed_manifest), "sha256": digest}
            try:
                atomic_json(index_path, index)
            except Exception:
                shutil.rmtree(destination)
                raise
    result = {"ok": True, "installed": True, "package": package["package"],
              "version": package["version"], "manifest": str(installed_manifest), "index": str(index_path)}
    if args.runtime_socket:
        if not args.instance_id:
            raise ValueError("--instance-id is required when applying a plugin")
        selected = args.class_type or plugin["plugins"][0]["class_type"]
        try:
            applied = request(args.runtime_socket, {
                "op": "load", "manifest": str(installed_manifest), "replace": True,
                "plugin": {"instance_id": args.instance_id, "device_id": args.device_id,
                           "component_id": args.component_id, "class_type": selected,
                           "api_version": API_VERSION, "auto_activate": True,
                           "config": json.loads(args.config)},
            })
        except Exception as exception:
            applied = {"ok": False, "message": str(exception)}
        result["applied"] = applied
        result["ok"] = bool(applied.get("ok"))
        # Failed activation leaves a valid installed package available for diagnosis/retry.
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    operations = parser.add_subparsers(dest="operation", required=True)
    install_parser = operations.add_parser("install")
    install_parser.add_argument("uri")
    install_parser.add_argument("--store", type=Path, default=Path.home() / ".local/share/uran/plugins")
    install_parser.add_argument("--sha256")
    install_parser.add_argument("--sdk-prefix", type=Path)
    install_parser.add_argument("--jobs", type=int, default=2)
    install_parser.add_argument("--runtime-socket")
    install_parser.add_argument("--instance-id")
    install_parser.add_argument("--device-id", default="robot_001")
    install_parser.add_argument("--component-id", default="extension")
    install_parser.add_argument("--class-type")
    install_parser.add_argument("--config", default="{}", help="Plugin-private JSON object")
    control_parser = operations.add_parser("request")
    control_parser.add_argument("--socket", required=True)
    control_parser.add_argument("json", help="JSON management command")
    pack_parser = operations.add_parser("pack")
    pack_parser.add_argument("directory", type=Path)
    pack_parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.operation == "install":
            if args.jobs < 1 or (args.runtime_socket and not args.instance_id):
                raise ValueError("positive --jobs and explicit --instance-id for runtime application are required")
            if not isinstance(json.loads(args.config), dict):
                raise ValueError("--config must be a JSON object")
            result = install(args)
        elif args.operation == "request":
            result = request(args.socket, json.loads(args.json))
        else:
            directory = args.directory.resolve()
            output = args.output.resolve()
            if output.is_relative_to(directory):
                raise ValueError("archive output must be outside the source directory")
            validate_package(load_json(directory / "uran-package.json"))
            with tarfile.open(output, "w:gz") as archive:
                for path in sorted(directory.rglob("*")):
                    if path.is_symlink():
                        raise ValueError("package sources must not contain symlinks")
                    archive.add(path, arcname=str(path.relative_to(directory)), recursive=False)
            result = {"ok": True, "archive": str(output), "sha256": hashlib.sha256(output.read_bytes()).hexdigest()}
        print(json.dumps(result, ensure_ascii=False))
        return 0 if result.get("ok") else 1
    except Exception as exception:
        print(json.dumps({"ok": False, "message": str(exception)}, ensure_ascii=False))
        return 1


if __name__ == "__main__":
    sys.exit(main())
