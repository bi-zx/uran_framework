"""Local release production, user approval and one-time device provisioning."""
import argparse
import getpass
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid

from .common import atomic_json, digest, read_json, rpc
from .security import sign


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="op")
    keygen = sub.add_parser("keygen")
    keygen.add_argument("--private-key", required=True)
    keygen.add_argument("--public-key", required=True)
    signing = sub.add_parser("sign")
    signing.add_argument("payload")
    signing.add_argument("--private-key", required=True)
    signing.add_argument("--key-id", required=True)
    signing.add_argument("--output", required=True)
    command = sub.add_parser("command")
    command.add_argument("--device-id", required=True)
    command.add_argument("--actor-id", required=True)
    command.add_argument("--action", required=True)
    command.add_argument("--args", required=True, help="JSON argument file")
    command.add_argument("--request-id", default=None)
    command.add_argument("--ttl", type=int, default=300)
    command.add_argument("--output", required=True)
    approve = sub.add_parser("approve")
    approve.add_argument("request", help="signed agent command")
    approve.add_argument("--private-key", required=True)
    approve.add_argument("--key-id", required=True)
    approve.add_argument("--ttl", type=int, default=600)
    approve.add_argument("--output", required=True)
    request = sub.add_parser("request")
    request.add_argument("envelope")
    request.add_argument("--socket", required=True)
    release = sub.add_parser("build-release")
    release.add_argument("source")
    release.add_argument("--spec", required=True)
    release.add_argument("--output", required=True)
    release.add_argument("--url", required=True)
    release.add_argument("--private-key", required=True)
    release.add_argument("--key-id", required=True)
    bundle = sub.add_parser("build-bootstrap")
    bundle.add_argument("--sdk-prefix", required=True)
    bundle.add_argument("--python-site", required=True)
    bundle.add_argument("--output", required=True)
    bundle.add_argument("--private-key", required=True)
    bundle.add_argument("--key-id", required=True)
    bundle.add_argument("--artifact-url", required=True)
    bundle.add_argument("--source-ref", required=True)
    install = sub.add_parser("install")
    install.add_argument("bundle", help="locally extracted, trusted bootstrap bundle")
    install.add_argument("--config", required=True)
    install.add_argument("--root", required=True)
    install.add_argument("--prefix", required=True)
    install.add_argument("--trusted-key", required=True)
    install.add_argument("--python", default=sys.executable)
    install.add_argument("--service-user", help="existing non-root account; install and enable systemd service")
    supervisor = sub.add_parser("supervise")
    supervisor.add_argument("--config", required=True)
    enrollment = sub.add_parser("enroll")
    enrollment.add_argument("--endpoint", required=True)
    enrollment.add_argument("--config", required=True)
    enrollment.add_argument("--ca-file")
    args = parser.parse_args(argv)
    if not args.op:
        parser.error("a command is required")
    try:
        if args.op == "keygen":
            for path in (args.private_key, args.public_key):
                if Path(path).exists():
                    raise ValueError("key destination already exists")
            old = os.umask(0o077)
            try:
                subprocess.run(["openssl", "genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:3072",
                                "-out", args.private_key], check=True, stderr=subprocess.PIPE)
                subprocess.run(["openssl", "pkey", "-in", args.private_key, "-pubout", "-out", args.public_key],
                               check=True, stderr=subprocess.PIPE)
            finally:
                os.umask(old)
        elif args.op == "sign":
            atomic_json(args.output, sign(read_json(args.payload), args.private_key, args.key_id))
        elif args.op == "command":
            now = int(time.time())
            body = {"schema_version": "1.0", "kind": "command", "device_id": args.device_id,
                    "actor_id": args.actor_id, "request_id": args.request_id or str(uuid.uuid4()),
                    "issued_at": now, "expires_at": now + args.ttl,
                    "action": args.action, "args": read_json(args.args)}
            atomic_json(args.output, body)
        elif args.op == "approve":
            envelope = read_json(args.request)
            body = envelope["payload"]
            # The user-facing cloud UI must show the same complete body before
            # its user signing service calls this operation. It must authenticate
            # the human independently of the agent's credentials.
            now = int(time.time())
            grant = {"schema_version": "1.0", "kind": "grant", "grant_id": str(uuid.uuid4()),
                     "device_id": body["device_id"], "actor_id": body["actor_id"],
                     "request_sha256": digest(body), "issued_at": now, "expires_at": now + args.ttl}
            envelope["grant"] = sign(grant, args.private_key, args.key_id)
            atomic_json(args.output, envelope)
        elif args.op == "request":
            result = rpc(args.socket, read_json(args.envelope), timeout=15)
            print(json.dumps(result, ensure_ascii=False, indent=2))
            return 0 if result.get("ok") else 1
        elif args.op == "build-release":
            from .packaging import build_release
            result = build_release(args.source, read_json(args.spec), args.output, args.url, args.private_key, args.key_id)
            print(json.dumps({"release_id": result["payload"]["release_id"],
                              "sha256": result["payload"]["artifact"]["sha256"]}))
        elif args.op == "build-bootstrap":
            from .packaging import build_bootstrap
            print(json.dumps(build_bootstrap(args.sdk_prefix, args.python_site, args.output,
                args.private_key, args.key_id, args.artifact_url, args.source_ref)))
        elif args.op == "install":
            from .bootstrap import install
            print(install(args.bundle, args.config, args.root, args.prefix, args.trusted_key,
                          args.python, args.service_user))
        elif args.op == "supervise":
            from .server import serve
            serve(args.config)
        elif args.op == "enroll":
            from .bootstrap import enroll
            config = read_json(args.config)
            code = getpass.getpass("一次性设备注册码: ")
            config["cloud"] = enroll(args.endpoint, code, config["device_id"], args.ca_file)
            atomic_json(args.config, config)
            print("设备凭据已写入本地配置")
        return 0
    except Exception as error:
        print("uran-deploy: " + (str(error) if isinstance(error, ValueError) else type(error).__name__), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
