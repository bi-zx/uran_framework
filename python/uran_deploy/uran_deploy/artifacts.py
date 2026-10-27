"""HTTPS artifact retrieval and exact, immutable release verification."""
import os
from pathlib import Path
import platform
import re
import shutil
import ssl
import sys
import tarfile
import time
import urllib.parse
import urllib.request

from .common import file_hash, identifier, read_json, relative_path, sync_dir

SHA = re.compile(r"^[0-9a-f]{64}$")


def origin(url):
    parsed = urllib.parse.urlsplit(url)
    if (parsed.scheme != "https" or not parsed.hostname or parsed.username or
            parsed.password or parsed.fragment):
        raise ValueError("artifacts require an HTTPS URL without userinfo or fragment")
    return "https://%s:%d" % (parsed.hostname.lower(), parsed.port or 443)


def validate_source(artifact, policy):
    if set(artifact) != {"url", "sha256", "size"} or not SHA.fullmatch(str(artifact.get("sha256", ""))):
        raise ValueError("invalid artifact descriptor")
    size = artifact.get("size")
    if type(size) is not int or not 0 < size <= policy.get("max_archive_bytes", 1024 ** 3):
        raise ValueError("artifact size exceeds policy")
    if not isinstance(artifact["url"], str) or len(artifact["url"]) > 8192:
        raise ValueError("invalid artifact URL length")
    if origin(artifact["url"]) not in {origin(url) for url in policy.get("artifact_origins", [])}:
        raise ValueError("artifact origin is not permitted by local policy")


class Redirects(urllib.request.HTTPRedirectHandler):
    def __init__(self, artifact, policy):
        self.artifact, self.policy = artifact, policy

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        validate_source(dict(self.artifact, url=newurl), self.policy)
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def download(artifact, cache, policy):
    validate_source(artifact, policy)
    cache = Path(cache)
    cache.mkdir(parents=True, exist_ok=True)
    target = cache / (artifact["sha256"] + ".tar")
    if target.exists():
        if target.stat().st_size == artifact["size"] and file_hash(target) == artifact["sha256"]:
            return target
        target.unlink()
    partial = target.with_suffix(".partial")
    context = ssl.create_default_context(cafile=policy.get("artifact_ca_file"))
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}),
        urllib.request.HTTPSHandler(context=context), Redirects(artifact, policy))
    deadline = time.monotonic() + policy.get("transfer_timeout_s", 600)
    for attempt in range(3):
        try:
            offset = partial.stat().st_size if partial.exists() else 0
            if offset >= artifact["size"]:
                if offset == artifact["size"] and file_hash(partial) == artifact["sha256"]:
                    os.replace(str(partial), str(target))
                    sync_dir(cache)
                    return target
                partial.unlink()
                offset = 0
            headers = {"Accept-Encoding": "identity"}
            if offset:
                headers["Range"] = "bytes=%d-" % offset
            with opener.open(urllib.request.Request(artifact["url"], headers=headers), timeout=15) as response:
                if response.status == 206:
                    expected = "bytes %d-%d/%d" % (offset, artifact["size"] - 1, artifact["size"])
                    if response.headers.get("Content-Range") != expected:
                        raise ValueError("invalid artifact range response")
                elif response.status == 200:
                    offset = 0
                else:
                    raise ValueError("unexpected artifact response")
                with open(str(partial), "ab" if offset else "wb") as output:
                    count = offset
                    while True:
                        if time.monotonic() >= deadline:
                            raise TimeoutError("artifact transfer deadline exceeded")
                        chunk = response.read(256 * 1024)
                        if not chunk:
                            break
                        count += len(chunk)
                        if count > artifact["size"]:
                            raise ValueError("artifact exceeds signed size")
                        output.write(chunk)
                    output.flush()
                    os.fsync(output.fileno())
            if count != artifact["size"]:
                raise OSError("incomplete artifact transfer")
            if file_hash(partial) != artifact["sha256"]:
                partial.unlink()
                raise ValueError("artifact SHA-256 mismatch")
            os.replace(str(partial), str(target))
            sync_dir(cache)
            return target
        except (OSError, TimeoutError):
            if attempt == 2 or time.monotonic() >= deadline:
                # Do not expose presigned URLs in remote errors or audit logs.
                raise ValueError("artifact transfer failed; partial download retained")
    raise ValueError("artifact transfer failed")


def extract(archive, destination, policy):
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=False)
    seen, total = set(), 0
    with tarfile.open(str(archive), "r:*") as bundle:
        for member in bundle:
            name = member.name.rstrip("/") if member.isdir() else member.name
            path = relative_path(destination, name)
            if name in seen or not (member.isfile() or member.isdir()):
                raise ValueError("duplicate or unsupported release archive member")
            seen.add(name)
            total += member.size
            if len(seen) > policy.get("max_files", 100000) or total > policy.get("max_unpacked_bytes", 4 * 1024 ** 3):
                raise ValueError("release extraction exceeds policy")
            if member.isdir():
                path.mkdir(parents=True, exist_ok=True)
                continue
            path.parent.mkdir(parents=True, exist_ok=True)
            with bundle.extractfile(member) as source, open(str(path), "xb") as output:
                shutil.copyfileobj(source, output, 256 * 1024)
                output.flush()
                os.fsync(output.fileno())
            path.chmod(0o755 if member.mode & 0o111 else 0o644)


def platform_check(release):
    expected = release.get("platform", {})
    if not isinstance(expected, dict) or set(expected) - {"os", "arch", "os_version", "distribution", "libc", "python"}:
        raise ValueError("unsupported platform requirements")
    if expected.get("os") != platform.system() or expected.get("arch") != platform.machine():
        raise ValueError("release platform does not match device OS/architecture")
    if release.get("api_version") != "2.1" or release.get("native_abi") != 3:
        raise ValueError("unsupported runtime API; bootstrap upgrade required")
    if expected.get("python") and expected["python"] != "%d.%d" % sys.version_info[:2]:
        raise ValueError("release Python version does not match the management interpreter")
    if expected.get("os_version") or expected.get("distribution"):
        values = {}
        path = Path("/etc/os-release")
        if path.exists():
            for line in path.read_text().splitlines():
                if "=" in line:
                    key, val = line.split("=", 1)
                    values[key] = val.strip('"')
        if expected.get("os_version") and values.get("VERSION_ID") != expected["os_version"]:
            raise ValueError("release OS version does not match device")
        if expected.get("distribution") and values.get("ID") != expected["distribution"]:
            raise ValueError("release OS distribution does not match device")
    if expected.get("libc") and list(platform.libc_ver()) != expected["libc"]:
        raise ValueError("release libc does not match device")


def validate_release(root, release, policy):
    root = Path(root)
    if root.is_symlink() or not root.is_dir():
        raise ValueError("release root must be a real directory")
    if (root / "deployment.lock.json").stat().st_size > 16 * 1024 ** 2:
        raise ValueError("deployment lock exceeds limit")
    lock = read_json(root / "deployment.lock.json")
    required = {"schema_version", "release_id", "profile_id", "profile_version", "api_version",
                "state_schema", "platform", "host", "runtime_config", "packages", "files",
                "services", "checks", "preflight", "post_activate", "migration", "native_abi"}
    if set(lock) != required or lock["schema_version"] != "1.0":
        raise ValueError("unsupported or incomplete release lock")
    for name in ("schema_version", "release_id", "profile_id", "profile_version", "api_version", "native_abi", "state_schema", "platform"):
        if lock[name] != release[name]:
            raise ValueError("signed release and lock differ: " + name)
    platform_check(release)
    expected = {"deployment.lock.json"}
    for entry in lock["files"]:
        if set(entry) != {"path", "sha256", "size", "executable"}:
            raise ValueError("invalid locked file")
        path = relative_path(root, entry["path"])
        if entry["path"] in expected or not SHA.fullmatch(str(entry["sha256"])):
            raise ValueError("duplicate or invalid locked file")
        expected.add(entry["path"])
        if (not path.is_file() or path.is_symlink() or path.stat().st_size != entry["size"] or
                file_hash(path) != entry["sha256"] or
                bool(path.stat().st_mode & 0o111) != entry["executable"]):
            raise ValueError("locked release file failed verification: " + entry["path"])
    actual = set()
    for path in root.rglob("*"):
        if path.is_symlink():
            raise ValueError("release contains a symbolic link")
        if path.is_file():
            actual.add(path.relative_to(root).as_posix())
    if actual != expected:
        raise ValueError("release contains unlisted or missing files")
    ids = set()
    if not lock["packages"]:
        raise ValueError("release lock must record package provenance")
    for package in lock["packages"]:
        if set(package) != {"package", "version", "origin", "sha256"}:
            raise ValueError("invalid package provenance record")
        name = identifier(package["package"])
        if name in ids or not package["version"] or not package["origin"] or not SHA.fullmatch(package["sha256"]):
            raise ValueError("incomplete or duplicate package provenance")
        ids.add(name)
    for field in ("host", "runtime_config"):
        path = relative_path(root, lock[field])
        if not path.is_file() or lock[field] not in expected:
            raise ValueError("missing " + field)
    if not os.access(str(root / lock["host"]), os.X_OK):
        raise ValueError("runtime host is not executable")
    # Canonical JSON is also valid YAML. Requiring JSON here avoids a PyYAML
    # dependency in the trusted bootstrap and makes package validation portable.
    runtime = read_json(root / lock["runtime_config"])
    validate_assembly(runtime, root)
    validate_services(lock["services"], root)
    for check in lock["checks"] + lock["preflight"] + lock["post_activate"]:
        validate_command(check, root)
    migration = lock["migration"]
    if migration is not None:
        if set(migration) != {"from_schema", "strategy", "command", "timeout_s"} or migration["strategy"] != "snapshot":
            raise ValueError("only snapshot-reversible file-state migrations are supported")
        identifier(migration["from_schema"])
        validate_command({"command": migration["command"], "timeout_s": migration["timeout_s"]}, root)
    return lock


def validate_command(spec, root):
    if set(spec) != {"command", "timeout_s"} or type(spec["timeout_s"]) is not int or not 0 < spec["timeout_s"] <= 300:
        raise ValueError("invalid bounded release command")
    command = spec["command"]
    if not isinstance(command, list) or not command or any(not isinstance(arg, str) or "\x00" in arg for arg in command):
        raise ValueError("release command must be an argv array")
    if not os.access(str(relative_path(root, command[0])), os.X_OK):
        raise ValueError("release command must execute a packaged file")


def validate_services(services, root):
    graph = {}
    for service in services:
        if set(service) != {"id", "command", "depends_on", "healthcheck"}:
            raise ValueError("invalid service definition")
        name = identifier(service["id"])
        if name in graph or name in {"core", "checks"} or not isinstance(service["depends_on"], list):
            raise ValueError("duplicate service or invalid dependencies")
        validate_command({"command": service["command"], "timeout_s": 30}, root)
        validate_command(service["healthcheck"], root)
        graph[name] = service["depends_on"]
    order = []
    while len(order) < len(graph):
        ready = sorted(name for name, deps in graph.items() if name not in order and all(dep in order for dep in deps))
        if not ready:
            raise ValueError("service dependencies contain a cycle or a missing service")
        order.extend(ready)
    return order


def validate_assembly(runtime, root):
    plugins = runtime.get("plugins", [])
    ids = set()
    for item in plugins:
        name = identifier(item["instance_id"])
        if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", name) or name in ids or name in {"uran_management", "uran_cloud", "uran_runtime"}:
            raise ValueError("duplicate or reserved plugin instance")
        if item.get("device_id", "${device_id}") != "${device_id}":
            raise ValueError("release plugin identities must use the local ${device_id}")
        if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", item.get("component_id", "")):
            raise ValueError("plugin component ID is missing or invalid")
        ids.add(name)
    components = runtime.get("components", [])
    graph = {}
    for component in components:
        name = identifier(component["component_id"])
        if name in graph:
            raise ValueError("duplicate component")
        graph[name] = component.get("parent_component_id")
        instances = component.get("instances", [])
        if any(instance not in ids for instance in instances):
            raise ValueError("component references an unknown plugin instance")
        provided = {capability for item in plugins if item["instance_id"] in instances
                    for capability in item.get("config", {}).get("capabilities", [])}
        if not set(component.get("requires", [])) <= provided:
            raise ValueError("component is missing a required capability")
    for name in graph:
        seen, cursor = set(), name
        while cursor is not None:
            if cursor in seen or cursor not in graph:
                raise ValueError("component parents contain a cycle or missing parent")
            seen.add(cursor)
            cursor = graph[cursor]
    classes = {"uran_worker_adapter/WorkerProcessPlugin", "uran_worker_adapter/WorkerProtocolPlugin"}
    for manifest in runtime.get("plugin_manifests", []):
        if manifest.startswith("${release}/"):
            path = relative_path(root, manifest[len("${release}/"):])
            contents = read_json(path)
            if contents.get("api_version") != "2.1" or contents.get("schema_version") != 1:
                raise ValueError("unsupported packaged plugin manifest")
            for plugin in contents.get("plugins", []):
                library = Path(plugin["library"])
                if library.is_absolute():
                    raise ValueError("packaged libraries must have relative paths")
                target = (path.parent / library).resolve()
                target.relative_to(Path(root).resolve())
                if not target.is_file():
                    raise ValueError("packaged plugin library is missing")
                classes.add(plugin["class_type"])
        else:
            raise ValueError("plugin manifests must be packaged under ${release}")
    if any(item.get("class_type") not in classes for item in plugins):
        raise ValueError("runtime references an unpackaged plugin class")


def freeze(root):
    for path in Path(root).rglob("*"):
        if path.is_file():
            path.chmod(0o555 if path.stat().st_mode & 0o111 else 0o444)
        elif path.is_dir():
            path.chmod(0o555)
    Path(root).chmod(0o555)
    for path in sorted(Path(root).rglob("*"), reverse=True):
        if path.is_dir():
            sync_dir(path)
    sync_dir(root)


def remove_tree(root):
    root = Path(root)
    if root.exists():
        for path in root.rglob("*"):
            if path.is_dir():
                path.chmod(0o700)
        root.chmod(0o700)
        shutil.rmtree(str(root))
