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
if [[ -f "${project_root}/install/setup.bash" ]]; then
  source "${project_root}/install/setup.bash"
fi
set -u

cd "${project_root}"
colcon test --event-handlers console_direct+
colcon test-result --verbose
