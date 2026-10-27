import platform
import sys

import pytest

from uran_deploy.artifacts import platform_check
from uran_deploy.bootstrap import install


def test_python_and_distribution_must_match_device():
    release = {"api_version": "2.1", "native_abi": 3,
        "platform": {"os": platform.system(), "arch": platform.machine(),
                     "python": "%d.%d" % sys.version_info[:2]}}
    platform_check(release)
    release["platform"]["python"] = "0.0"
    with pytest.raises(ValueError, match="Python version"):
        platform_check(release)
    release["platform"]["python"] = "%d.%d" % sys.version_info[:2]
    release["platform"]["distribution"] = "not-the-device-distribution"
    with pytest.raises(ValueError, match="OS distribution"):
        platform_check(release)


def test_install_rejects_a_different_service_interpreter(config, keys, https, tmp_path, sdk, python_site):
    from uran_deploy.artifacts import extract
    from uran_deploy.common import atomic_json
    from uran_deploy.packaging import build_bootstrap

    archive = tmp_path / "bootstrap.tar.gz"
    build_bootstrap(sdk, python_site, archive, keys["publisher"][0], "publisher",
                    https["url"] + "/seed.tar.gz", "test:installed-sdk")
    bundle = tmp_path / "bundle"
    extract(archive, bundle, {})
    config_path = tmp_path / "device.json"
    atomic_json(config_path, config)
    # A launcher reporting another interpreter version exercises the rejection
    # before any target directory or system service can be installed.
    interpreter = tmp_path / "other-python"
    interpreter.write_text("#!/bin/sh\nprintf '0.0\\n'\n")
    interpreter.chmod(0o755)
    with pytest.raises(ValueError, match="selected service interpreter"):
        install(bundle, config_path, config["root"], config["bootstrap_prefix"],
                keys["publisher"][1], python=str(interpreter))
    assert not (tmp_path / "device").exists()
    assert not (tmp_path / "bootstrap").exists()
