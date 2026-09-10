from pathlib import Path

from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch_ros.actions import Node


def generate_launch_description():
    """启动 URAN Runtime 基础框架。"""
    share = Path(get_package_share_directory("uran_core_framework"))

    return LaunchDescription([
        Node(
            package="uran_core_framework",
            executable="uran_runtime_node",
            name="uran_runtime",
            output="screen",
            parameters=[{
                "route_config": str(share / "config" / "routes.yaml"),
                "plugin_config": str(share / "config" / "plugins.yaml"),
            }],
        ),
    ])
