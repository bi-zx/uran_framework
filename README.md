# URAN Core Framework

当前目录是 URAN 的 C++/ROS 2 原型，用于验证插件生命周期、多实例、状态隔离、能力注册和多协议装配。当前实现仍依赖 `rclcpp`、`pluginlib` 和 ROS 2 构建体系，不代表最终的平台无关架构。

目标架构由平台无关的 `UranRuntime`、领域服务和通用插件 API 组成，ROS 2 通过独立的 RuntimeHost、Platform Services 和 Loader 后端接入。未来接入其他机器人操作系统时，应新增或替换平台适配器，而不修改设备模型、领域语义和通用插件生命周期。

- `uran_plugin_api`：当前原型的插件模型和基类，不依赖 Runtime 实现，但仍含 ROS 2 类型，后续需要提取平台无关接口。
- `uran_core_framework`：当前原型的状态空间、能力注册表、路由目录、消息路由、协议网关、插件管理器和 RuntimeNode。
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

当前原型已经验证：

- 统一的 C++ 插件生命周期。
- 插件实现描述与运行实例身份分离。
- PluginContext 提供实例、设备、组件身份和受控 Runtime 能力。
- 同一插件实现类可创建多个独立实例。
- 插件状态写入和注销权限与运行实例标识绑定。
- 线程安全的状态存储。
- 按设备、组件和能力组织的能力注册表。
- 配置驱动的路由目录。
- 协议插件抽象。
- 多协议实例选择、优先级、上行回退和带来源身份的下行消息路由。
- 基于 pluginlib 的插件发现、加载和管理。
- 基于 `rclcpp` 的 RuntimeNode 骨架。

这些能力后续需要拆分为平台无关 Runtime 与 ROS 2 Adapter。通用核心不能暴露 `rclcpp::Node`、ROS 消息或 pluginlib 类型；ROS Namespace、Topic、Service、Action 和 Executor 等平台概念由 ROS 2 Adapter 管理。

具体 ROS 消息和服务接口应在 ROS 2 适配接口设计阶段单独确定。当前 Runtime 示例节点使用 `std_msgs/msg/String` 作为占位数据通道，不代表最终的 URAN 领域模型或业务接口。

当前代码已经把设备、组件、插件实现、插件实例和能力拆分：同一个实现类可创建多个独立实例，一个实例可提供多项能力，一个设备可同时装配多条协议通道。真实协议连接、正式领域接口和具体机器人适配仍需在此框架上继续实现。详见 [插件化架构说明](docs/插件化架构说明.md) 和 [目前框架说明](docs/目前框架说明.md)。

## 当前 ROS 2 原型构建

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

运行时是否加载插件由参数 `plugin_config` 指向的 YAML 文件决定。bringup 示例会用同一个回环协议实现类创建主、备两个实例，用于验证实例身份、状态隔离和协议选择；回环协议不代表系统已经接入真实云端。
