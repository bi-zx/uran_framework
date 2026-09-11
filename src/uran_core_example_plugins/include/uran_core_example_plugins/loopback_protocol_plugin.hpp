#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "uran_plugin_api/protocol_plugin.hpp"

namespace uran_core_example_plugins
{

/**
 * @brief 不访问外部网络的回环协议插件。
 *
 * 仅用于演示协议插件的配置、生命周期和回调方式。
 */
class LoopbackProtocolPlugin final : public uran_plugin_api::ProtocolPlugin
{
public:
  LoopbackProtocolPlugin() = default;
  ~LoopbackProtocolPlugin() override = default;

  uran_plugin_api::PluginDescriptor descriptor() const override;
  uran_plugin_api::Status configure(
    const uran_plugin_api::PluginContext & context,
    const YAML::Node & config) override;
  uran_plugin_api::Status activate() override;
  uran_plugin_api::Status deactivate() override;
  uran_plugin_api::Status cleanup() override;

  uran_plugin_api::PluginHealth health() const override;
  uran_plugin_api::PluginCapabilities capabilities() const override;

  uran_plugin_api::Status connect() override;
  uran_plugin_api::Status disconnect() override;
  uran_plugin_api::Status publish(
    const uran_plugin_api::UplinkEnvelope & message) override;
  void set_downlink_callback(
    uran_plugin_api::DownlinkCallback callback) override;

private:
  mutable std::mutex mutex_;
  rclcpp::Logger logger_{rclcpp::get_logger("loopback_protocol")};
  std::shared_ptr<uran_plugin_api::StateAccess> state_access_;
  uran_plugin_api::DownlinkCallback downlink_callback_;
  uran_plugin_api::PluginIdentity identity_;
  std::string connected_state_field_;
  std::string channel_name_{"default"};
  bool configured_{false};
  bool active_{false};
  bool connected_{false};
  std::uint64_t error_count_{0};
};

}
