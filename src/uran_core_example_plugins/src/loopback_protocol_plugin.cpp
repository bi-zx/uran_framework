#include "uran_core_example_plugins/loopback_protocol_plugin.hpp"

#include <chrono>
#include <exception>
#include <utility>

#include <pluginlib/class_list_macros.hpp>

namespace uran_core_example_plugins
{

uran_plugin_api::PluginDescriptor LoopbackProtocolPlugin::descriptor() const
{
  return uran_plugin_api::PluginDescriptor{
    "example_loopback_protocol",
    "protocol",
    "0.1.0",
    "1.0",
    "uran_core_example_plugins/LoopbackProtocolPlugin"};
}

uran_plugin_api::Status LoopbackProtocolPlugin::configure(
  const uran_plugin_api::PluginContext & context,
  const YAML::Node & config)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (configured_) {
    return uran_plugin_api::Status::failure("回环协议插件已经配置");
  }

  try {
    channel_name_ = config["channel"] ? config["channel"].as<std::string>() : "default";
  } catch (const YAML::Exception & exception) {
    ++error_count_;
    return uran_plugin_api::Status::failure(
      std::string("回环协议配置无效: ") + exception.what());
  }
  if (channel_name_.empty()) {
    ++error_count_;
    return uran_plugin_api::Status::failure("回环通道名称不能为空");
  }

  const auto node = context.node();
  if (!node) {
    ++error_count_;
    return uran_plugin_api::Status::failure("Runtime 节点已经失效");
  }
  logger_ = node->get_logger();
  state_access_ = context.state_access();
  const auto registered = state_access_->register_field(
    uran_plugin_api::StateFieldSpec{
      "plugin.example_loopback_protocol.connected",
      "bool",
      "",
      descriptor().plugin_id,
      false});
  if (!registered.ok) {
    state_access_.reset();
    ++error_count_;
    return registered;
  }
  configured_ = true;
  RCLCPP_INFO(logger_, "回环协议插件配置完成，通道=%s", channel_name_.c_str());
  return uran_plugin_api::Status::success();
}

uran_plugin_api::Status LoopbackProtocolPlugin::activate()
{
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!configured_) {
      return uran_plugin_api::Status::failure("插件尚未配置");
    }
    if (active_) {
      return uran_plugin_api::Status::success("插件已经激活");
    }
    active_ = true;
  }
  return connect();
}

uran_plugin_api::Status LoopbackProtocolPlugin::deactivate()
{
  const auto result = disconnect();
  std::lock_guard<std::mutex> lock(mutex_);
  active_ = false;
  return result;
}

uran_plugin_api::Status LoopbackProtocolPlugin::cleanup()
{
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_ || connected_) {
      return uran_plugin_api::Status::failure("清理插件前必须先停用插件");
    }
    downlink_callback_ = nullptr;
    state_access_.reset();
    channel_name_ = "default";
    configured_ = false;
  }
  return uran_plugin_api::Status::success();
}

uran_plugin_api::PluginHealth LoopbackProtocolPlugin::health() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  uran_plugin_api::PluginHealth result;
  result.checked_at = std::chrono::system_clock::now();
  result.error_count = error_count_;
  if (connected_) {
    result.level = uran_plugin_api::PluginHealth::Level::healthy;
    result.message = "回环协议已连接";
  } else if (active_) {
    result.level = uran_plugin_api::PluginHealth::Level::degraded;
    result.message = "插件已激活但协议未连接";
  } else if (configured_) {
    result.level = uran_plugin_api::PluginHealth::Level::stopped;
    result.message = "插件已配置但未激活";
  } else {
    result.level = uran_plugin_api::PluginHealth::Level::unknown;
    result.message = "插件尚未配置";
  }
  return result;
}

uran_plugin_api::PluginCapabilities LoopbackProtocolPlugin::capabilities() const
{
  return uran_plugin_api::PluginCapabilities{
    {"protocol.loopback", "protocol.uplink", "protocol.downlink"},
    {},
    {"plugin.example_loopback_protocol.connected"}};
}

uran_plugin_api::Status LoopbackProtocolPlugin::connect()
{
  std::shared_ptr<uran_plugin_api::StateAccess> state_access;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!configured_ || !active_) {
      return uran_plugin_api::Status::failure("插件未处于可连接状态");
    }
    connected_ = true;
    state_access = state_access_;
  }
  const auto updated = state_access->set(
    "plugin.example_loopback_protocol.connected", "true", descriptor().plugin_id);
  if (!updated.ok) {
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = false;
    ++error_count_;
    return updated;
  }
  RCLCPP_INFO(logger_, "回环协议已连接");
  return uran_plugin_api::Status::success();
}

uran_plugin_api::Status LoopbackProtocolPlugin::disconnect()
{
  std::shared_ptr<uran_plugin_api::StateAccess> state_access;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = false;
    state_access = state_access_;
  }
  if (state_access) {
    const auto updated = state_access->set(
      "plugin.example_loopback_protocol.connected", "false", descriptor().plugin_id);
    if (!updated.ok) {
      std::lock_guard<std::mutex> lock(mutex_);
      ++error_count_;
      return updated;
    }
  }
  RCLCPP_INFO(logger_, "回环协议已断开");
  return uran_plugin_api::Status::success();
}

uran_plugin_api::Status LoopbackProtocolPlugin::publish(
  const uran_plugin_api::UplinkEnvelope & message)
{
  uran_plugin_api::DownlinkCallback callback;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_) {
      ++error_count_;
      return uran_plugin_api::Status::failure("回环协议尚未连接");
    }
    callback = downlink_callback_;
  }

  // 外部回调必须在插件锁外执行，允许接收方安全地回调插件其他方法。
  if (callback) {
    callback(uran_plugin_api::DownlinkEnvelope{
      message.message_type, message.payload, message.correlation_id});
  }
  return uran_plugin_api::Status::success();
}

void LoopbackProtocolPlugin::set_downlink_callback(
  uran_plugin_api::DownlinkCallback callback)
{
  std::lock_guard<std::mutex> lock(mutex_);
  downlink_callback_ = std::move(callback);
}

}

PLUGINLIB_EXPORT_CLASS(
  uran_core_example_plugins::LoopbackProtocolPlugin,
  uran_plugin_api::Plugin)
