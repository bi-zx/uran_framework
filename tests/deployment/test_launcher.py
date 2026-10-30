import os
import subprocess
import sys

from uran_deploy.packaging import launcher


def test_launcher_never_writes_bytecode_into_the_package(tmp_path):
    package = tmp_path / "python" / "probe_package"
    package.mkdir(parents=True)
    (package / "__init__.py").write_text("import json\n\ndef main():\n    return 7\n")
    script = tmp_path / "bin" / "probe"
    script.parent.mkdir()
    script.write_text(launcher("probe_package"))
    # As documented: plain python3, no PYTHONDONTWRITEBYTECODE in the environment.
    environment = {key: value for key, value in os.environ.items() if not key.startswith("PYTHON")}
    assert subprocess.run([sys.executable, str(script)], env=environment).returncode == 7
    assert not list(tmp_path.rglob("__pycache__"))
