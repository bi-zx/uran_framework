#!/usr/bin/env bash
set -euo pipefail
project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${URAN_BUILD_DIR:-${project_root}/build}"
install_prefix="${URAN_INSTALL_PREFIX:-${project_root}/install}"
cmake -S "${project_root}" -B "${build_dir}" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX="${install_prefix}" \
  "$@"
cmake --build "${build_dir}" --parallel "${URAN_BUILD_JOBS:-2}"
cmake --install "${build_dir}"
