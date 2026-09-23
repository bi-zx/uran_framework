from __future__ import annotations

import importlib
import sys

from .protocol import Worker, WorkerServer


def main() -> int:
    if len(sys.argv) != 2 or ":" not in sys.argv[1]:
        print("usage: python -m uran_worker_sdk module:factory", file=sys.stderr)
        return 2
    module_name, factory_name = sys.argv[1].split(":", 1)
    factory = getattr(importlib.import_module(module_name), factory_name)
    worker = factory()
    if not isinstance(worker, Worker):
        raise TypeError("factory must return uran_worker_sdk.Worker")
    return WorkerServer(worker).serve()


if __name__ == "__main__":
    raise SystemExit(main())
