"""Linux process guardian: bind lifetime to the supervisor process, not a thread.

PR_SET_PDEATHSIG tracks the spawning Linux thread, which is unsuitable when a
deployment worker thread starts a long-lived Core. A small guardian also bounds
the lifetime of check/migration descendants without that false death signal.
"""
import os
import ctypes
import signal
import subprocess
import sys
import time


def identity(pid):
    try:
        with open("/proc/%d/stat" % pid) as stream:
            return stream.read().rsplit(")", 1)[1].split()[19]
    except (OSError, IndexError):
        return None


def descendants():
    records = {}
    for name in os.listdir("/proc"):
        if not name.isdigit():
            continue
        try:
            with open("/proc/%s/stat" % name) as stream:
                fields = stream.read().rsplit(")", 1)[1].split()
            records[int(name)] = (int(fields[1]), fields[19])
        except (OSError, IndexError, ValueError):
            continue
    selected = {os.getpid()}
    while True:
        more = {pid for pid, (parent, _start) in records.items() if parent in selected}
        if more <= selected:
            break
        selected.update(more)
    return [(pid, records[pid][1]) for pid in selected if pid != os.getpid()]


def cleanup():
    for sig, duration in ((signal.SIGTERM, 1.0), (signal.SIGKILL, 0.5)):
        children = descendants()
        for pid, started in children:
            if identity(pid) == started:
                try:
                    os.kill(pid, sig)
                except ProcessLookupError:
                    pass
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            try:
                while os.waitpid(-1, os.WNOHANG)[0]:
                    pass
            except ChildProcessError:
                return
            time.sleep(0.02)


def main():
    parent = int(sys.argv[1])
    parent_identity = identity(parent)
    if not parent_identity or os.getppid() != parent:
        return 1
    libc = ctypes.CDLL(None, use_errno=True)
    # Adopt double-forked descendants too, including processes that start their
    # own session. Each guardian is a separate subreaper for its managed tree.
    if libc.prctl(36, 1, 0, 0, 0):
        raise OSError(ctypes.get_errno(), "PR_SET_CHILD_SUBREAPER failed")
    stopping = [False]
    for signum in (signal.SIGTERM, signal.SIGINT):
        signal.signal(signum, lambda *_: stopping.__setitem__(0, True))
    process = subprocess.Popen(sys.argv[2:], stdin=subprocess.DEVNULL)
    while (not stopping[0] and identity(parent) == parent_identity and process.poll() is None):
        time.sleep(0.1)
    try:
        if process.poll() is None:
            # Give Core the chance to deactivate its hardware plugins first.
            process.terminate()
            try:
                process.wait(timeout=7)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=1)
        return process.returncode
    finally:
        cleanup()


if __name__ == "__main__":
    sys.exit(main())
