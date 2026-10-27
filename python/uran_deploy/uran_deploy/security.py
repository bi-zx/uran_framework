"""Separate agent, human approval and publisher trust domains."""
import base64
from pathlib import Path
import subprocess
import tempfile
import time

from .common import canonical, digest, identifier


def _openssl(arguments, data=None):
    result = subprocess.run(["openssl"] + arguments, input=data, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, timeout=15)
    if result.returncode:
        raise ValueError("signature operation failed")
    return result.stdout


def sign(payload, private_key, key_id):
    signature = _openssl(["dgst", "-sha256", "-sign", str(private_key)], canonical(payload))
    return {"algorithm": "rsa-sha256", "key_id": identifier(key_id), "payload": payload,
            "signature": base64.b64encode(signature).decode("ascii")}


class Authority:
    def __init__(self, config, root, clock=time.time):
        self.config = config
        self.root = Path(root)
        self.clock = clock

    def verify(self, envelope, roles, kind):
        allowed = {"algorithm", "key_id", "payload", "signature"}
        if kind == "command":
            allowed.add("grant")
        if not isinstance(envelope, dict) or set(envelope) - allowed:
            raise ValueError("invalid signed envelope")
        if envelope.get("algorithm") != "rsa-sha256":
            raise ValueError("unsupported signature algorithm")
        key = self.config.get("trust", {}).get(envelope.get("key_id"))
        if not key or key.get("role") not in roles or key.get("disabled", False):
            raise ValueError("untrusted signing key or signing role")
        payload = envelope.get("payload")
        if not isinstance(payload, dict) or payload.get("schema_version") != "1.0" or payload.get("kind") != kind:
            raise ValueError("unsupported signed payload")
        try:
            signature = base64.b64decode(envelope["signature"], validate=True)
        except (ValueError, TypeError, KeyError):
            raise ValueError("invalid signature encoding")
        with tempfile.TemporaryDirectory(prefix="uran-verify-") as directory:
            signature_path = Path(directory) / "signature"
            signature_path.write_bytes(signature)
            _openssl(["dgst", "-sha256", "-verify", key["public_key"],
                      "-signature", str(signature_path)], canonical(payload))
        return payload, key

    def lifetime(self, payload, limit):
        issued, expires = payload.get("issued_at"), payload.get("expires_at")
        if type(issued) is not int or type(expires) is not int:
            raise ValueError("full Unix seconds are required for authorization timestamps")
        now = self.clock()
        if issued > now + 30 or expires <= now or expires <= issued or expires - issued > limit:
            raise ValueError("authorization expired or outside permitted lifetime")

    def command(self, envelope):
        body, key = self.verify(envelope, {"agent", "user"}, "command")
        if set(body) != {"schema_version", "kind", "request_id", "device_id", "actor_id",
                         "issued_at", "expires_at", "action", "args"}:
            raise ValueError("unexpected or missing command fields")
        identifier(body["request_id"])
        if body["device_id"] != self.config["device_id"] or body["actor_id"] != key["subject"]:
            raise ValueError("command device or actor does not match trusted identity")
        self.lifetime(body, self.config.get("policy", {}).get("max_task_ttl_s", 300))
        if not isinstance(body["args"], dict):
            raise ValueError("command args must be an object")
        return body, key

    def release(self, envelope):
        release, key = self.verify(envelope, {"publisher"}, "release")
        required = {"schema_version", "kind", "release_id", "profile_id", "profile_version",
                    "api_version", "native_abi", "state_schema", "platform", "artifact"}
        if set(release) != required:
            raise ValueError("unexpected or missing release fields")
        for name in ("release_id", "profile_id", "profile_version", "state_schema"):
            identifier(release[name])
        return release, key

    def authorize(self, envelope, body, key, resource, publisher=None, revoked=(), used=None):
        """Return the approval grant, or None for a matching local auto policy."""
        action = body["action"]
        if key["role"] == "user" and action != "maintenance.open":
            return None
        if action.startswith("deployment."):
            for rule in self.config.get("policy", {}).get("auto_deploy", []):
                if (resource.get("profile_id") == rule.get("profile_id") and
                        body["actor_id"] in rule.get("agents", []) and
                        publisher in rule.get("publishers", []) and
                        action in rule.get("actions", ["deployment.apply"])):
                    return None
        grant, human = self.verify(envelope.get("grant"), {"user"}, "grant")
        del human
        if set(grant) != {"schema_version", "kind", "grant_id", "device_id", "actor_id",
                          "request_sha256", "issued_at", "expires_at"}:
            raise ValueError("unexpected or missing grant fields")
        grant_id = identifier(grant["grant_id"])
        self.lifetime(grant, self.config.get("policy", {}).get("max_grant_ttl_s", 900))
        if (grant["device_id"] != body["device_id"] or grant["actor_id"] != body["actor_id"] or
                grant["request_sha256"] != digest(body)):
            raise ValueError("approval does not match the concrete request")
        if grant_id in revoked or (used or {}).get(grant_id, body["request_id"]) != body["request_id"]:
            raise ValueError("approval was revoked or already used")
        return grant
