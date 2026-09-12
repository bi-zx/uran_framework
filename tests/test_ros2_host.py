"""Installed ROS adapter smoke test. Run with ROS sourced; pass the install prefix."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

prefix = Path(sys.argv[1]).resolve()
os.environ["AMENT_PREFIX_PATH"] = str(prefix) + os.pathsep + os.environ.get("AMENT_PREFIX_PATH", "")
os.environ["ROS_DOMAIN_ID"] = str(30 + os.getpid() % 170)
os.environ["ROS_LOCALHOST_ONLY"] = "1"

import rclpy
from std_msgs.msg import String
from std_srvs.srv import Trigger

rclpy.init()
node = rclpy.create_node("uran_adapter_test")
received = {"heartbeat": [], "downlink": [], "reply": [], "snapshot": []}
subscriptions = []
for key, topic in (("heartbeat", "heartbeat"), ("downlink", "downlink"),
                   ("reply", "management/reply"), ("snapshot", "state_snapshot")):
    subscriptions.append(node.create_subscription(
        String, "/uran/runtime/" + topic,
        lambda message, key=key: received[key].append(json.loads(message.data)), 10))
uplink = node.create_publisher(String, "/uran/runtime/uplink", 10)
management = node.create_publisher(String, "/uran/runtime/management/request", 10)
service = node.create_client(Trigger, "/uran/runtime/state_report")

process = subprocess.Popen([
    str(prefix / "lib/uran_ros2_adapter/uran_runtime_node"), "--ros-args",
    "-p", "plugin_config:=" + str(prefix / "share/uran_framework/config/loopback.yaml"),
    "-p", "heartbeat_period_ms:=100", "-p", "snapshot_period_ms:=100"],
    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def until(predicate):
    deadline = time.monotonic() + 12
    while not predicate():
        if process.poll() is not None:
            raise AssertionError(process.communicate())
        if time.monotonic() >= deadline:
            raise AssertionError("ROS adapter response timed out: " + repr(received))
        rclpy.spin_once(node, timeout_sec=0.05)


def send(publisher, body):
    until(lambda: publisher.get_subscription_count() > 0)
    publisher.publish(String(data=json.dumps(body)))


try:
    until(lambda: received["heartbeat"] and received["heartbeat"][-1]["ready"])
    assert received["heartbeat"][-1]["protocol_instance_count"] == 2
    send(uplink, {"message_type": "framework.echo", "payload": {"value": 42}, "correlation_id": "first"})
    until(lambda: any(m["correlation_id"] == "first" for m in received["downlink"]))
    assert received["downlink"][-1]["source"]["instance_id"] == "protocol_primary"
    send(management, {"op": "deactivate", "instance_id": "protocol_primary", "correlation_id": "stop"})
    until(lambda: any(m.get("correlation_id") == "stop" for m in received["reply"]))
    assert received["reply"][-1]["ok"]
    send(uplink, {"message_type": "framework.echo", "payload": {}, "correlation_id": "backup"})
    until(lambda: any(m["correlation_id"] == "backup" for m in received["downlink"]))
    assert received["downlink"][-1]["source"]["instance_id"] == "protocol_backup"
    assert service.wait_for_service(timeout_sec=5)
    response = service.call_async(Trigger.Request())
    until(response.done)
    assert response.result().success
    until(lambda: received["snapshot"])
    assert received["snapshot"][-1]["system.ready"]
finally:
    if process.poll() is None:
        process.send_signal(signal.SIGINT)
    try:
        output, errors = process.communicate(timeout=8)
    except subprocess.TimeoutExpired:
        process.kill()
        output, errors = process.communicate()
    node.destroy_node()
    rclpy.shutdown()
assert process.returncode == 0, (output, errors)
print("ROS adapter: pluginlib loading, heartbeat, source identity, live management, backup and snapshot passed")
