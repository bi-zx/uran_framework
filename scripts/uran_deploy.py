#!/usr/bin/env python3
"""Source-tree and installed-SDK entry point."""
from pathlib import Path
import sys

prefix = Path(__file__).resolve().parents[1]
source = prefix / "python/uran_deploy"
installed = prefix / "share/uran_framework/python/uran_deploy"
sys.path.insert(0, str(source if source.is_dir() else installed))
from uran_deploy.cli import main

if __name__ == "__main__":
    raise SystemExit(main())
