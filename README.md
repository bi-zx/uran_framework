# URAN Core Framework

这是 URAN 的 C++/ROS 2 基础框架骨架，用于承载后续核心运行时和领域插件。

- `uran_plugin_api`：独立、稳定的插件模型和基类，不依赖 Runtime 实现。
- `uran_core_framework`：状态空间、路由目录、消息路由、协议网关、插件管理器和 Runtime 节点。
- `uran_core_example_plugins`：一个最小的协议插件，用于验证 `pluginlib` 接口和生命周期。
- `uran_bringup`：集中提供系统配置和启动装配。

## 目录

```text
uran_rebuild/
├── docs/
├── scripts/
├── README.md
└── src/
    ├── uran_core_framework/
    ├── uran_core_example_plugins/
    ├── uran_plugin_api/
    └── uran_bringup/
```

## 设计边界

框架层只负责机制：

- 统一的 C++ 插件生命周期。
- PluginContext 受控访问 Runtime 公共服务。
- 插件状态写权限与插件 ID 绑定。
- 线程安全的状态存储。
- 配置驱动的路由目录。
- 协议插件抽象。
- 多协议选择、上行回退和下行消息路由。
- pluginlib 插件发现、加载和管理。
- 基于 `rclcpp` 的 Runtime 节点骨架。

具体 ROS 消息和服务接口应在公共接口设计阶段单独确定。当前 Runtime 示例节点使用 `std_msgs/msg/String` 作为占位数据通道，不代表最终的 URAN 业务接口。

## 构建

```bash
source /opt/ros/humble/setup.bash
cd uran_rebuild
rosdep install --from-paths src --ignore-src -r -y
colcon build --cmake-args -DCMAKE_BUILD_TYPE=RelWithDebInfo
source install/setup.bash
```

也可以使用项目脚本完成依赖安装、构建和测试：

```bash
bash scripts/build.sh
bash scripts/test.sh
```

运行框架节点：

```bash
ros2 run uran_core_framework uran_runtime_node
```

使用 bringup 加载示例协议插件：

```bash
ros2 launch uran_bringup uran_framework.launch.py
```

示例插件会将占位上行消息回送到占位下行通道，可用于检查基础链路：

```bash
ros2 topic echo /uran/runtime/downlink
ros2 topic pub --once /uran/runtime/uplink std_msgs/msg/String \
  "{data: '{\"message_type\":\"framework.echo\",\"payload\":{\"value\":1}}'}"
```

运行时是否加载插件由参数 `plugin_config` 指向的 YAML 文件决定。插件包中的示例配置默认关闭示例插件，避免在没有实际协议实现时误认为系统已经接入云端。
