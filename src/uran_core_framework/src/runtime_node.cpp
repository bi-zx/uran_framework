#include "uran_core_framework/runtime_node.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace uran_core_framework
{

namespace
{

constexpr const char * kRuntimeOwner = "uran_runtime";

}

RuntimeNode::RuntimeNode(const rclcpp::NodeOptions & options)
: Node("uran_runtime", options),
  state_store_(std::make_shared<StateStore>(true)),
  route_catalog_(std::make_shared<RouteCatalog>()),
  message_router_(std::make_shared<MessageRouter>()),
  protocol_gateway_(std::make_shared<ProtocolGateway>()),
  started_at_(std::chrono::steady_clock::now())
{
  route_config_path_ = declare_parameter<std::string>("route_config", "");
  plugin_config_path_ = declare_parameter<std::string>("plugin_config", "");
  snapshot_period_ms_ = declare_parameter<int>("snapshot_period_ms", 1000);
  heartbeat_period_ms_ = declare_parameter<int>("heartbeat_period_ms", 5000);

  snapshot_period_ms_ = std::max(snapshot_period_ms_, 100);
  heartbeat_period_ms_ = std::max(heartbeat_period_ms_, 100);

  static_cast<void>(route_catalog_->set("state_snapshot", "/uran/runtime/state_snapshot"));
  static_cast<void>(route_catalog_->set("heartbeat", "/uran/runtime/heartbeat"));
  static_cast<void>(route_catalog_->set("state_write", "/uran/runtime/state_write"));
  static_cast<void>(route_catalog_->set("state_report", "/uran/runtime/state_report"));
  static_cast<void>(route_catalog_->set("uplink", "/uran/runtime/uplink"));
  static_cast<void>(route_catalog_->set("downlink", "/uran/runtime/downlink"));

  if (!route_config_path_.empty()) {
    const auto loaded = route_catalog_->load_file(route_config_path_);
    if (!loaded.ok) {
      throw std::runtime_error(loaded.message);
    }
  }

  register_core_state_fields();
  static_cast<void>(state_store_->set("system.ready", "false", kRuntimeOwner));
  static_cast<void>(state_store_->set("system.uptime_seconds", "0", kRuntimeOwner));
  static_cast<void>(state_store_->set("plugin.count", "0", kRuntimeOwner));
  static_cast<void>(state_store_->set("network.protocol_count", "0", kRuntimeOwner));
  static_cast<void>(state_store_->set("network.primary_protocol", "\"\"", kRuntimeOwner));

  snapshot_publisher_ = create_publisher<std_msgs::msg::String>(
    route_or("state_snapshot", "/uran/runtime/state_snapshot"), 10);
  heartbeat_publisher_ = create_publisher<std_msgs::msg::String>(
    route_or("heartbeat", "/uran/runtime/heartbeat"), 10);
  downlink_publisher_ = create_publisher<std_msgs::msg::String>(
    route_or("downlink", "/uran/runtime/downlink"), 10);
  runtime_subscription_ = create_subscription<std_msgs::msg::String>(
    route_or("state_write", "/uran/runtime/state_write"), 10,
    std::bind(&RuntimeNode::on_runtime_text, this, std::placeholders::_1));
  uplink_subscription_ = create_subscription<std_msgs::msg::String>(
    route_or("uplink", "/uran/runtime/uplink"), 10,
    std::bind(&RuntimeNode::on_uplink_text, this, std::placeholders::_1));
  trigger_service_ = create_service<std_srvs::srv::Trigger>(
    route_or("state_report", "/uran/runtime/state_report"),
    std::bind(
      &RuntimeNode::on_trigger, this,
      std::placeholders::_1, std::placeholders::_2));

  snapshot_timer_ = create_wall_timer(
    std::chrono::milliseconds(snapshot_period_ms_),
    std::bind(&RuntimeNode::publish_snapshot, this));
  heartbeat_timer_ = create_wall_timer(
    std::chrono::milliseconds(heartbeat_period_ms_),
    std::bind(&RuntimeNode::publish_heartbeat, this));

  message_router_->set_unhandled_handler(
    std::bind(&RuntimeNode::publish_unhandled_downlink, this, std::placeholders::_1));
  const std::weak_ptr<MessageRouter> weak_router = message_router_;
  protocol_gateway_->set_downlink_handler(
    [weak_router](const uran_plugin_api::DownlinkEnvelope & message) {
      if (const auto router = weak_router.lock()) {
        static_cast<void>(router->route(message));
      }
    });

  RCLCPP_INFO(get_logger(), "URAN Runtime 基础组件已完成装配");
}

RuntimeNode::~RuntimeNode()
{
  if (message_router_) {
    message_router_->set_unhandled_handler(nullptr);
  }
  if (protocol_gateway_) {
    protocol_gateway_->set_downlink_handler(nullptr);
  }
  protocol_gateway_.reset();
  message_router_.reset();
  // 先销毁插件管理器
  plugin_manager_.reset();
  plugin_context_.reset();
}

Status RuntimeNode::initialize_plugins()
{
  if (plugins_initialized_) {
    return Status::success("插件系统已经初始化");
  }

  auto candidate_manager = std::make_unique<PluginManager>("uran_plugin_api");
  auto candidate_context = std::make_shared<uran_plugin_api::PluginContext>(
    shared_from_this(), state_store_, route_catalog_, get_namespace());
  std::string configured_primary_protocol;

  if (!plugin_config_path_.empty()) {
    const auto loaded = config_manager_.load_file(plugin_config_path_);
    if (!loaded.ok) {
      return loaded;
    }

    const auto root = config_manager_.snapshot();
    if (root["primary_protocol"]) {
      try {
        configured_primary_protocol = root["primary_protocol"].as<std::string>();
      } catch (const YAML::Exception & exception) {
        return Status::failure(
          std::string("主协议配置无效: ") + exception.what());
      }
    }
    const auto plugins = root["plugins"] ? root["plugins"] : root;
    const auto configured = candidate_manager->configure_and_load_all(
      plugins, *candidate_context);
    if (!configured.ok) {
      return configured;
    }
  }

  std::vector<std::string> registered_protocols;
  // 只有已激活的协议才能进入发送链路，避免调用仅完成配置的插件
  for (const auto & plugin : candidate_manager->active_instances()) {
    const auto protocol = std::dynamic_pointer_cast<uran_plugin_api::ProtocolPlugin>(plugin);
    if (!protocol) {
      continue;
    }
    uran_plugin_api::PluginDescriptor descriptor;
    try {
      descriptor = protocol->descriptor();
    } catch (const std::exception & exception) {
      for (const auto & protocol_id : registered_protocols) {
        static_cast<void>(protocol_gateway_->unregister_protocol(protocol_id));
      }
      return Status::failure(
        std::string("读取协议插件描述失败: ") + exception.what());
    } catch (...) {
      for (const auto & protocol_id : registered_protocols) {
        static_cast<void>(protocol_gateway_->unregister_protocol(protocol_id));
      }
      return Status::failure("读取协议插件描述时发生未知异常");
    }
    const auto registered = protocol_gateway_->register_protocol(
      descriptor.plugin_id, protocol);
    if (!registered.ok) {
      for (const auto & protocol_id : registered_protocols) {
        static_cast<void>(protocol_gateway_->unregister_protocol(protocol_id));
      }
      return registered;
    }
    registered_protocols.push_back(descriptor.plugin_id);
  }

  if (!configured_primary_protocol.empty()) {
    const auto selected = protocol_gateway_->set_primary(configured_primary_protocol);
    if (!selected.ok) {
      for (const auto & protocol_id : registered_protocols) {
        static_cast<void>(protocol_gateway_->unregister_protocol(protocol_id));
      }
      return selected;
    }
  }

  plugin_manager_ = std::move(candidate_manager);
  plugin_context_ = std::move(candidate_context);
  plugins_initialized_ = true;
  const auto plugin_count = plugin_manager_->list().size();
  static_cast<void>(state_store_->set(
      "plugin.count", std::to_string(plugin_count), kRuntimeOwner));
  const auto protocol_ids = protocol_gateway_->protocol_ids();
  static_cast<void>(state_store_->set(
      "network.protocol_count", std::to_string(protocol_ids.size()), kRuntimeOwner));
  static_cast<void>(state_store_->set(
      "network.primary_protocol",
      nlohmann::json(protocol_gateway_->primary_protocol()).dump(), kRuntimeOwner));
  static_cast<void>(state_store_->set("system.ready", "true", kRuntimeOwner));
  RCLCPP_INFO(get_logger(), "插件系统初始化完成，已加载 %zu 个插件", plugin_count);
  return Status::success("插件系统初始化完成");
}

void RuntimeNode::publish_snapshot()
{
  std_msgs::msg::String message;
  message.data = state_store_->snapshot_json();
  snapshot_publisher_->publish(message);
}

void RuntimeNode::publish_heartbeat()
{
  const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
    std::chrono::steady_clock::now() - started_at_).count();
  static_cast<void>(state_store_->set(
      "system.uptime_seconds", std::to_string(elapsed), kRuntimeOwner));

  nlohmann::json heartbeat;
  heartbeat["ready"] = plugins_initialized_;
  heartbeat["uptime_seconds"] = elapsed;
  heartbeat["plugin_count"] = plugin_manager_ ? plugin_manager_->list().size() : 0U;
  heartbeat["protocol_count"] = protocol_gateway_ ? protocol_gateway_->protocol_ids().size() : 0U;

  std_msgs::msg::String message;
  message.data = heartbeat.dump();
  heartbeat_publisher_->publish(message);
}

void RuntimeNode::on_runtime_text(const std_msgs::msg::String::SharedPtr message)
{
  if (!message) {
    return;
  }

  try {
    const auto payload = nlohmann::json::parse(message->data);
    if (!payload.is_object() || !payload.contains("field") || !payload.contains("value")) {
      RCLCPP_WARN(get_logger(), "忽略格式不完整的状态写入占位消息");
      return;
    }

    const auto field = payload.at("field").get<std::string>();
    // 占位 ROS 通道无法证明发布者身份，因此不能接受消息内自报的所有者
    const std::string source = "external_ros";
    const bool persistent = payload.value("persistent", false);
    const auto status = state_store_->set(
      field, payload.at("value").dump(), source, persistent);
    if (!status.ok) {
      RCLCPP_WARN(get_logger(), "状态写入被拒绝: %s", status.message.c_str());
    }
  } catch (const nlohmann::json::exception & exception) {
    RCLCPP_WARN(get_logger(), "状态写入占位消息解析失败: %s", exception.what());
  }
}

void RuntimeNode::on_uplink_text(const std_msgs::msg::String::SharedPtr message)
{
  if (!message || !protocol_gateway_) {
    return;
  }

  try {
    const auto payload = nlohmann::json::parse(message->data);
    if (!payload.is_object() || !payload.contains("message_type") ||
      !payload.contains("payload"))
    {
      RCLCPP_WARN(get_logger(), "忽略格式不完整的上行占位消息");
      return;
    }

    uran_plugin_api::UplinkEnvelope envelope;
    envelope.message_type = payload.at("message_type").get<std::string>();
    envelope.payload = payload.at("payload").is_string() ?
      payload.at("payload").get<std::string>() : payload.at("payload").dump();
    envelope.correlation_id = payload.value("correlation_id", std::string{});
    envelope.urgent = payload.value("urgent", false);
    const auto preferred = payload.value("preferred_protocol", std::string{});

    const auto sent = protocol_gateway_->publish(envelope, preferred);
    if (!sent.ok) {
      RCLCPP_WARN(get_logger(), "上行占位消息发送失败: %s", sent.message.c_str());
    }
  } catch (const nlohmann::json::exception & exception) {
    RCLCPP_WARN(get_logger(), "上行占位消息解析失败: %s", exception.what());
  }
}

void RuntimeNode::publish_unhandled_downlink(
  const uran_plugin_api::DownlinkEnvelope & message)
{
  nlohmann::json payload;
  payload["message_type"] = message.message_type;
  payload["correlation_id"] = message.correlation_id;
  auto body = nlohmann::json::parse(message.payload, nullptr, false);
  payload["payload"] = body.is_discarded() ? nlohmann::json(message.payload) : body;

  std_msgs::msg::String output;
  output.data = payload.dump();
  downlink_publisher_->publish(output);
}

void RuntimeNode::on_trigger(
  const std_srvs::srv::Trigger::Request::SharedPtr request,
  std_srvs::srv::Trigger::Response::SharedPtr response)
{
  static_cast<void>(request);
  if (!response) {
    return;
  }
  publish_snapshot();
  response->success = true;
  response->message = "状态快照已发布";
}

std::string RuntimeNode::route_or(
  const std::string & name, const std::string & fallback) const
{
  std::string route;
  return route_catalog_->resolve(name, route) ? route : fallback;
}

void RuntimeNode::register_core_state_fields()
{
  static_cast<void>(state_store_->register_field(
      StateFieldSpec{"system.ready", "bool", "", kRuntimeOwner, false}));
  static_cast<void>(state_store_->register_field(
      StateFieldSpec{
        "system.uptime_seconds", "int64", "s", kRuntimeOwner, false}));
  static_cast<void>(state_store_->register_field(
      StateFieldSpec{"plugin.count", "uint64", "", kRuntimeOwner, false}));
  static_cast<void>(state_store_->register_field(
      StateFieldSpec{
        "network.protocol_count", "uint64", "", kRuntimeOwner, false}));
  static_cast<void>(state_store_->register_field(
      StateFieldSpec{
        "network.primary_protocol", "string", "", kRuntimeOwner, false}));
}

}
