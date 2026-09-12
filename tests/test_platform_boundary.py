from pathlib import Path
import re
import sys
root = Path(sys.argv[1])
for package in ("uran_plugin_api", "uran_core_framework", "uran_core_example_plugins"):
    for path in (root / "src" / package).rglob("*"):
        if path.suffix not in (".hpp", ".cpp") or "test" in path.parts:
            continue
        content = path.read_text()
        assert not re.search(r"#\s*include\s*[<\"](?:rclcpp|rclpy|pluginlib|std_msgs|std_srvs|sys/|unistd|dlfcn)", content), path
        assert not re.search(r"\b(?:rclcpp|rclpy|pluginlib)::", content), path
print("Public API, core and generic plugin contain no platform SDK/OS dependencies")
