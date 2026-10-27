import os
from pathlib import Path
import subprocess
import sys

from uran_deploy import exec_child


def test_guardian_reaps_detached_background_descendants(tmp_path):
    script = tmp_path / "spawn.py"
    pidfile = tmp_path / "background.pid"
    script.write_text("import subprocess,sys\n"
        "from pathlib import Path\n"
        "p=subprocess.Popen([sys.executable,'-c','import time; time.sleep(60)'],start_new_session=True)\n"
        "Path(sys.argv[1]).write_text(str(p.pid))\n")
    result = subprocess.run([sys.executable, exec_child.__file__, str(os.getpid()),
        sys.executable, str(script), str(pidfile)], start_new_session=True, timeout=10)
    assert result.returncode == 0
    assert not Path("/proc/" + pidfile.read_text()).exists(), "detached grandchild escaped managed lifetime"
