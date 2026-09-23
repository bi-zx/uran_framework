#include "uran_core_example_plugins/loopback_protocol_plugin.hpp"

#include <chrono>
#include <exception>
#include <utility>

#include "uran_plugin_api/plugin_loader.hpp"

namespace uran_core_example_plugins
{

uran_plugin_api::PluginDescriptor LoopbackProtocolPlugin::descriptor() const
{
  return uran_plugin_api::PluginDescriptor{
    "protocol",
    "0.1.0",
    uran_plugin_api::kPluginApiVersion,
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

  platform_ = context.platform();
  identity_ = context.identity();
  connected_state_field_ = "plugin." + identity_.instance_id + ".connected";
  state_access_ = context.state_access();
  const auto registered = state_access_->register_field(
    uran_plugin_api::StateFieldSpec{
      connected_state_field_,
      "bool",
      "",
      "",
      false});
  if (!registered.ok) {
    state_access_.reset();
    identity_ = {};
    connected_state_field_.clear();
    ++error_count_;
    return registered;
  }
  const auto initialized = state_access_->set(
    connected_state_field_, "false", identity_.instance_id);
  if (!initialized.ok) {
    static_cast<void>(state_access_->unregister_field(
        connected_state_field_, identity_.instance_id));
    state_access_.reset();
    identity_ = {};
    connected_state_field_.clear();
    ++error_count_;
    return initialized;
  }
  configured_ = true;
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
    if (state_access_ && !connected_state_field_.empty()) {
      const auto unregistered = state_access_->unregister_field(
        connected_state_field_, identity_.instance_id);
      if (!unregistered.ok) {
        ++error_count_;
        return unregistered;
      }
    }
    downlink_callback_ = nullptr;
    state_access_.reset();
    identity_ = {};
    connected_state_field_.clear();
    channel_name_ = "default";
    configured_ = false;
    error_count_ = 0;
  }
  return uran_plugin_api::Status::success();
}

uran_plugin_api::PluginHealth LoopbackProtocolPlugin::health() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  uran_plugin_api::PluginHealth result;
  result.checked_at = platform_ ? platform_->system_now() : std::chrono::system_clock::time_point{};
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
  std::lock_guard<std::mutex> lock(mutex_);
  return uran_plugin_api::PluginCapabilities{
    {"protocol.loopback", "protocol.uplink", "protocol.downlink"},
    {},
    connected_state_field_.empty() ? std::vector<std::string>{} :
    std::vector<std::string>{connected_state_field_}};
}

uran_plugin_api::Status LoopbackProtocolPlugin::connect()
{
  std::shared_ptr<uran_plugin_api::StateAccess> state_access;
  std::string state_field;
  std::string instance_id;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!configured_ || !active_) {
      return uran_plugin_api::Status::failure("插件未处于可连接状态");
    }
    connected_ = true;
    state_access = state_access_;
    state_field = connected_state_field_;
    instance_id = identity_.instance_id;
  }
  const auto updated = state_access->set(
    state_field, "true", instance_id);
  if (!updated.ok) {
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = false;
    ++error_count_;
    return updated;
  }
  return uran_plugin_api::Status::success();
}

uran_plugin_api::Status LoopbackProtocolPlugin::disconnect()
{
  std::shared_ptr<uran_plugin_api::StateAccess> state_access;
  std::string state_field;
  std::string instance_id;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = false;
    state_access = state_access_;
    state_field = connected_state_field_;
    instance_id = identity_.instance_id;
  }
  if (state_access) {
    const auto updated = state_access->set(
      state_field, "false", instance_id);
    if (!updated.ok) {
      std::lock_guard<std::mutex> lock(mutex_);
      ++error_count_;
      return updated;
    }
  }
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
      message.message_type, message.payload, message.correlation_id, {}, {}, 0});
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

extern "C" URAN_PLUGIN_API_EXPORT const uran_plugin_api::NativePluginExport * uran_plugin_export_v3()
{
  static const uran_plugin_api::NativePluginExport exported{
    uran_plugin_api::kNativePluginAbiVersion,
    "uran_core_example_plugins/LoopbackProtocolPlugin",
    []() -> uran_plugin_api::Plugin * {return new uran_core_example_plugins::LoopbackProtocolPlugin;},
    [](uran_plugin_api::Plugin * plugin) noexcept {delete plugin;}};
  return &exported;
}
