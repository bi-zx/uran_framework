#!/usr/bin/env bash
set -euo pipefail
project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${URAN_BUILD_DIR:-${project_root}/build}"
ctest --test-dir "${build_dir}" --output-on-failure "$@"
