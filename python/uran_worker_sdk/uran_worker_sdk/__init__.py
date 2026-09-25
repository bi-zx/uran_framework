"""Reference SDK for URAN process workers using uran.worker.jsonl.v1."""

import time

from .protocol import (
    PROTOCOL,
    TERMINAL_STATES,
    Error,
    Worker,
    WorkerServer,
)


def now_ns():
    """Return wall-clock nanoseconds on Python versions without time.time_ns."""
    return int(time.time() * 1000000000)

__all__ = ["PROTOCOL", "TERMINAL_STATES", "Error", "Worker", "WorkerServer", "now_ns"]
