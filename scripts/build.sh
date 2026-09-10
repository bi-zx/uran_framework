#!/usr/bin/env bash

set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ros_distribution="${ROS_DISTRO:-humble}"
ros_setup="/opt/ros/${ros_distribution}/setup.bash"

if [[ ! -f "${ros_setup}" ]]; then
  echo "未找到 ROS 2 环境: ${ros_setup}" >&2
  exit 1
fi

set +u
source "${ros_setup}"
set -u

cd "${project_root}"
rosdep install --from-paths src --ignore-src -r -y
colcon build --cmake-args -DCMAKE_BUILD_TYPE=RelWithDebInfo
