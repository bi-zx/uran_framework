"""Bounded JSON, durable records and local-only RPC (Python 3.6+)."""
import contextlib
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import socket
import tempfile

MAX_FRAME = 1024 * 1024
ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{0,127}$")


def identifier(value):
    if not isinstance(value, str) or not ID.fullmatch(value):
        raise ValueError("invalid identifier")
    return value


def _pairs(items):
    result = {}
    for key, value in items:
        if key in result:
            raise ValueError("duplicate JSON key: " + key)
        result[key] = value
    return result


def loads(data):
    def finite(value):
        result = float(value)
        if not math.isfinite(result):
            raise ValueError("non-finite JSON")
        return result
    return json.loads(data, object_pairs_hook=_pairs, parse_float=finite,
                      parse_constant=lambda _: (_ for _ in ()).throw(ValueError("non-finite JSON")))


def read_json(path):
    return loads(Path(path).read_text(encoding="utf-8"))


def canonical(value):
    # Signed documents intentionally use integers only. This avoids Python/JS
    # floating point serialization differences. Configuration files are bytes.
    def check(item):
        if isinstance(item, dict):
            for key, val in item.items():
                if not isinstance(key, str):
                    raise ValueError("JSON object keys must be strings")
                check(val)
        elif isinstance(item, list):
            for val in item:
                check(val)
        elif item is not None and not isinstance(item, (str, bool, int)):
            raise ValueError("signed JSON only supports strings, integers, booleans and null")
    check(value)
    return json.dumps(value, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":"), allow_nan=False).encode("utf-8")


def digest(value):
    return hashlib.sha256(canonical(value)).hexdigest()


def file_hash(path):
    result = hashlib.sha256()
    with open(str(path), "rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def sync_dir(path):
    descriptor = os.open(str(path), os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix=".record-", dir=str(path.parent))
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            json.dump(value, stream, ensure_ascii=False, sort_keys=True, allow_nan=False)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(name, str(path))
        sync_dir(path.parent)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def relative_path(root, value):
    if not isinstance(value, str) or not value or "\\" in value or "\x00" in value:
        raise ValueError("invalid relative path")
    parts = PurePosixPath(value)
    if parts.is_absolute() or ".." in parts.parts or str(parts) != value or value == ".":
        raise ValueError("path must be normalized and relative: " + value)
    path = Path(root).joinpath(*parts.parts)
    path.resolve().relative_to(Path(root).resolve())
    return path


@contextlib.contextmanager
def exclusive(path):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    with open(str(path), "a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield


def rpc(path, value, timeout=5):
    data = json.dumps(value, separators=(",", ":"), allow_nan=False).encode() + b"\n"
    if len(data) > MAX_FRAME:
        raise ValueError("request exceeds frame limit")
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(timeout)
        client.connect(str(path))
        client.sendall(data)
        response = bytearray()
        while b"\n" not in response:
            chunk = client.recv(min(65536, MAX_FRAME + 1 - len(response)))
            if not chunk:
                raise ValueError("local RPC stream closed")
            response.extend(chunk)
            if len(response) > MAX_FRAME:
                raise ValueError("response exceeds frame limit")
    return loads(bytes(response).split(b"\n", 1)[0])
