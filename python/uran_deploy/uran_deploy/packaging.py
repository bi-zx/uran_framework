"""Offline release and bootstrap production from installed artifacts."""
from pathlib import Path
import platform
import shutil
import sys
import tarfile
import tempfile

from . import __version__
from .artifacts import validate_release, origin
from .common import atomic_json, digest, file_hash
from .security import sign


def inventory(root):
    result = []
    for path in sorted(Path(root).rglob("*")):
        if path.is_symlink():
            raise ValueError("packaged artifacts must not contain symlinks")
        if path.is_file():
            result.append({"path": path.relative_to(root).as_posix(), "sha256": file_hash(path),
                           "size": path.stat().st_size, "executable": bool(path.stat().st_mode & 0o111)})
    return result


def tar_tree(source, output):
    with tarfile.open(str(output), "w:gz", dereference=True) as archive:
        for path in sorted(Path(source).rglob("*")):
            archive.add(str(path), arcname=path.relative_to(source).as_posix(), recursive=False)


def build_release(source, spec, output, url, private_key, key_id):
    source, output = Path(source), Path(output)
    origin(url)
    if output.exists() or output.with_name(output.name + ".release.json").exists():
        raise ValueError("release output already exists")
    if source.resolve() in Path(private_key).resolve().parents:
        raise ValueError("publisher private key must not be inside release artifacts")
    if (source / "deployment.lock.json").exists():
        raise ValueError("source must not contain a generated deployment.lock.json")
    if output.resolve() == source.resolve() or source.resolve() in output.resolve().parents:
        raise ValueError("release output must be outside the source tree")
    output.parent.mkdir(parents=True, exist_ok=True)
    lock = dict(spec, schema_version="1.0", files=inventory(source))
    for key, default in (("services", []), ("checks", []), ("preflight", []), ("post_activate", []), ("migration", None), ("native_abi", 3)):
        lock.setdefault(key, default)
    with tempfile.TemporaryDirectory(prefix="uran-release-") as temporary:
        tree = Path(temporary) / "release"
        shutil.copytree(str(source), str(tree))
        atomic_json(tree / "deployment.lock.json", lock)
        release = {key: lock[key] for key in ("schema_version", "release_id", "profile_id",
                   "profile_version", "api_version", "native_abi", "state_schema", "platform")}
        release.update(kind="release", artifact={"url": url, "sha256": "0" * 64, "size": 1})
        validate_release(tree, release, {})
        tar_tree(tree, output)
    release["artifact"].update(sha256=file_hash(output), size=output.stat().st_size)
    envelope = sign(release, private_key, key_id)
    atomic_json(output.with_name(output.name + ".release.json"), envelope)
    return envelope


def launcher(module):
    return ("#!/usr/bin/env python3\nimport pathlib, sys\n"
            "sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'python'))\n"
            "from %s import main\nraise SystemExit(main())\n" % module)


def copy_runtime_sdk(source, destination):
    source, destination = Path(source), Path(destination)
    for relative in ("bin/uran_runtime", "share/uran_framework/plugins/worker/plugin.json"):
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(str(source / relative), str(target))
    for path in sorted((source / "lib").glob("*.so*")):
        if path.is_file():
            (destination / "lib").mkdir(parents=True, exist_ok=True)
            shutil.copy2(str(path.resolve()), str(destination / "lib" / path.name))


def build_bootstrap(sdk, python_site, output, private_key, key_id, artifact_url, source_ref):
    """No sibling repository imports, pip, builds or SSH on the target device."""
    output = Path(output)
    origin(artifact_url)
    if output.exists() or output.with_name(output.name + ".sig").exists():
        raise ValueError("bootstrap output already exists")
    for source in (Path(sdk).resolve(), Path(python_site).resolve()):
        if source in Path(private_key).resolve().parents or source in output.resolve().parents:
            raise ValueError("bootstrap key/output must be outside input artifacts")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="uran-bootstrap-") as directory:
        tree = Path(directory) / "bundle"
        tree.mkdir()
        copy_runtime_sdk(sdk, tree / "sdk")
        # python_site is a target-compatible, already installed dependency set.
        shutil.copytree(str(python_site), str(tree / "python"),
                        ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "bin"))
        for name in ("uran_worker_sdk", "uran_mqtt_plugin", "uran_interfaces", "paho"):
            if not (tree / "python" / name).is_dir():
                raise ValueError("bootstrap dependency set is missing " + name)
        target = tree / "python/uran_deploy"
        if target.exists():
            shutil.rmtree(str(target))
        shutil.copytree(str(Path(__file__).parent), str(target),
                        ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
        for name, module in (("uran-deploy", "uran_deploy.cli"),
                             ("uran-management-worker", "uran_deploy.worker"),
                             ("uran-cloud-worker", "uran_mqtt_plugin.worker"),
                             ("uran-exec", "uran_deploy.exec_child")):
            path = tree / "bin" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(launcher(module))
            path.chmod(0o755)
        source = Path(directory) / "seed"
        shutil.copytree(str(tree / "sdk"), str(source))
        (source / "config").mkdir()
        atomic_json(source / "config/runtime.json", {"plugins": [], "components": []})
        sdk_digest = digest(inventory(source))
        spec = {"release_id": "bootstrap-" + __version__, "profile_id": "management",
                "profile_version": __version__, "api_version": "2.1", "state_schema": "1",
                "platform": {"os": platform.system(), "arch": platform.machine(),
                             "libc": list(platform.libc_ver()), "python": "%d.%d" % sys.version_info[:2]},
                "host": "bin/uran_runtime", "runtime_config": "config/runtime.json",
                "packages": [{"package": "uran-framework", "version": "2.1.0",
                              "origin": source_ref, "sha256": sdk_digest}]}
        os_release = Path("/etc/os-release")
        if os_release.exists():
            values = dict(line.split("=", 1) for line in os_release.read_text().splitlines() if "=" in line)
            for key, field in (("ID", "distribution"), ("VERSION_ID", "os_version")):
                if values.get(key):
                    spec["platform"][field] = values[key].strip('"')
        build_release(source, spec, tree / "seed.tar.gz", artifact_url, private_key, key_id)
        manifest = {"schema_version": "1.0", "kind": "bootstrap", "version": __version__,
                    "platform": spec["platform"], "files": inventory(tree)}
        atomic_json(tree / "bootstrap.json", sign(manifest, private_key, key_id))
        tar_tree(tree, output)
    signature = output.with_name(output.name + ".sig")
    # Operator verifies this detached signature with their independently trusted
    # publisher key BEFORE extracting/executing anything from the bootstrap.
    result = __import__("subprocess").run(["openssl", "dgst", "-sha256", "-sign", str(private_key), str(output)],
                                          stdout=__import__("subprocess").PIPE, check=True)
    signature.write_bytes(result.stdout)
    return {"bundle": str(output), "sha256": file_hash(output), "signature": str(signature)}
