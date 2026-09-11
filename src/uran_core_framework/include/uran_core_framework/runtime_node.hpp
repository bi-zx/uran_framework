#pragma once

#include <chrono>
#include <memory>
#include <string>

#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>
#include <std_srvs/srv/trigger.hpp>

#include "uran_core_framework/capability_registry.hpp"
#include "uran_core_framework/config_manager.hpp"
#include "uran_core_framework/message_router.hpp"
#include "uran_core_framework/plugin_manager.hpp"
#include "uran_core_framework/protocol_gateway.hpp"
#include "uran_core_framework/route_catalog.hpp"
#include "uran_core_framework/state_store.hpp"
#include "uran_core_framework/visibility_control.hpp"
#include "uran_plugin_api/plugin_context.hpp"

namespace uran_core_framework
{

/**
 * @brief 最小可运行的 Runtime 装配节点。
 *
 * 暂时使用 String 作为占位通道
 */
class URAN_CORE_FRAMEWORK_PUBLIC RuntimeNode final : public rclcpp::Node
{
public:
  explicit RuntimeNode(const rclcpp::NodeOptions & options = rclcpp::NodeOptions());
  ~RuntimeNode() override;

  /** @brief 在节点已由 shared_ptr 托管后加载插件。 */
  Status initialize_plugins();

private:
  void publish_snapshot();
  void publish_heartbeat();
  void on_runtime_text(const std_msgs::msg::String::SharedPtr message);
  void on_uplink_text(const std_msgs::msg::String::SharedPtr message);
  void publish_unhandled_downlink(const uran_plugin_api::DownlinkEnvelope & message);
  void on_trigger(
    const std_srvs::srv::Trigger::Request::SharedPtr request,
    std_srvs::srv::Trigger::Response::SharedPtr response);

  std::string route_or(const std::string & name, const std::string & fallback) const;
  void register_core_state_fields();

  std::shared_ptr<StateStore> state_store_;
  std::shared_ptr<RouteCatalog> route_catalog_;
  std::shared_ptr<CapabilityRegistry> capability_registry_;
  ConfigManager config_manager_;
  std::unique_ptr<PluginManager> plugin_manager_;
  std::shared_ptr<uran_plugin_api::PluginContext> plugin_context_;
  std::shared_ptr<MessageRouter> message_router_;
  std::shared_ptr<ProtocolGateway> protocol_gateway_;

  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr snapshot_publisher_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr heartbeat_publisher_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr downlink_publisher_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr runtime_subscription_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr uplink_subscription_;
  rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr trigger_service_;
  rclcpp::TimerBase::SharedPtr snapshot_timer_;
  rclcpp::TimerBase::SharedPtr heartbeat_timer_;

  std::string route_config_path_;
  std::string plugin_config_path_;
  int snapshot_period_ms_{1000};
  int heartbeat_period_ms_{5000};
  bool plugins_initialized_{false};
  std::chrono::steady_clock::time_point started_at_;
};

}
