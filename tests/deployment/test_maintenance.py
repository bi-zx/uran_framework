from pathlib import Path
import sys
import time

import pytest

from uran_deploy.common import read_json
from uran_deploy.engine import DeploymentEngine
from uran_deploy.processes import process_identity
from test_transactions import approval, command, Controller


def test_disabled_tunnel_expiring_grant_and_human_revocation(config, keys, tmp_path):
    provider = tmp_path / "provider.py"
    provider.write_text('import json,sys,time\nrequest=json.loads(sys.stdin.readline())\n'
                        'print(json.dumps({"ok":True,"connection":{"state":"starting"}}),flush=True)\n'
                        'time.sleep(30)\n')
    config["maintenance"].update(provider_command=[sys.executable, str(provider)],
        services={"ssh": {}}, max_session_s=1)
    engine = DeploymentEngine(config, Controller(config))
    opening = command(keys, "maintenance.open", {"service_id": "ssh", "ssh_public_key": "fixture"}, "open")
    with pytest.raises(ValueError):
        engine.submit(opening)
    approval(keys, opening)
    with pytest.raises(ValueError, match="disabled"):
        engine.submit(opening)
    config["maintenance"]["enabled"] = True
    first = engine.submit(opening)
    assert first["result"]["state"] == "starting"
    pid = engine.maintenance.record["pid"]
    time.sleep(1.05)
    engine.maintenance.tick()
    assert engine.maintenance.record is None and process_identity(pid) is None
    second = approval(keys, command(keys, "maintenance.open", {"service_id": "ssh", "ssh_public_key": "fixture"}, "open-again"), grant_id="second")
    config["maintenance"]["max_session_s"] = 30
    engine.submit(second)
    assert engine.maintenance.record is not None
    with pytest.raises(ValueError, match="only a user"):
        engine.submit(command(keys, "authorization.revoke", {"grant_id": "second"}, "forged-revoke"))
    engine.submit(command(keys, "authorization.revoke", {"grant_id": "second"}, "revoke", "user"))
    assert engine.maintenance.record is None
    assert "second" in engine.ledger()["revoked"]
