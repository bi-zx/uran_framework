#!/usr/bin/env bash
set -euo pipefail
project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${URAN_BUILD_DIR:-${project_root}/build}"
install_prefix="${URAN_INSTALL_PREFIX:-${project_root}/install}"
ros_options=()
if [[ "${1:-}" == "--ros2" ]]; then
  shift
  ros_setup="/opt/ros/${ROS_DISTRO:-humble}/setup.bash"
  if [[ ! -f "${ros_setup}" ]]; then
    echo "未找到 ROS 2 环境: ${ros_setup}" >&2
    exit 1
  fi
  set +u
  source "${ros_setup}"
  set -u
  ros_options=(-DURAN_BUILD_ROS2=ON -DPython3_EXECUTABLE=/usr/bin/python3)
fi
cmake -S "${project_root}" -B "${build_dir}" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX="${install_prefix}" \
  "${ros_options[@]}" "$@"
cmake --build "${build_dir}" --parallel "${URAN_BUILD_JOBS:-2}"
cmake --install "${build_dir}"
