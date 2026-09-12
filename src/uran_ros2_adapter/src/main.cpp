#include <algorithm>
#include <memory>
#include <nlohmann/json.hpp>
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>
#include <std_srvs/srv/trigger.hpp>
#include "uran_core_framework/runtime.hpp"
#include "uran_platform_native/native_platform.hpp"
#include "uran_platform_native/file_state_storage.hpp"
#include "uran_ros2_adapter/pluginlib_loader.hpp"

namespace uran_ros2_adapter
{
using Json = nlohmann::json;
using Text = std_msgs::msg::String;

class Ros2Platform final : public uran_platform_native::NativePlatform
{
public:
  explicit Ros2Platform(const std::shared_ptr<rclcpp::Node> & node) : node_(node) {}
  void log(uran_plugin_api::LogLevel level, const std::string & source, const std::string & message) override
  {
    if (const auto node = node_.lock()) {
      const auto text = source + ": " + message;
      switch (level) {
        case uran_plugin_api::LogLevel::debug: RCLCPP_DEBUG(node->get_logger(), "%s", text.c_str()); break;
        case uran_plugin_api::LogLevel::info: RCLCPP_INFO(node->get_logger(), "%s", text.c_str()); break;
        case uran_plugin_api::LogLevel::warning: RCLCPP_WARN(node->get_logger(), "%s", text.c_str()); break;
        case uran_plugin_api::LogLevel::error: RCLCPP_ERROR(node->get_logger(), "%s", text.c_str()); break;
      }
    }
  }
  std::chrono::system_clock::time_point system_now() const override
  {
    if (const auto node = node_.lock()) {
      return std::chrono::system_clock::time_point(std::chrono::nanoseconds(node->now().nanoseconds()));
    }
    return NativePlatform::system_now();
  }
private:
  std::weak_ptr<rclcpp::Node> node_;
};

class Ros2RuntimeHost final : public rclcpp::Node
{
public:
  Ros2RuntimeHost() : Node("uran_runtime")
  {
    declare_parameter<std::string>("plugin_config", "");
    declare_parameter<std::string>("route_config", "");
    declare_parameter<std::string>("state_file", "");
    declare_parameter<int>("snapshot_period_ms", 1000);
    declare_parameter<int>("heartbeat_period_ms", 5000);
  }
  void initialize()
  {
    platform_ = std::make_shared<Ros2Platform>(shared_from_this());
    std::shared_ptr<uran_plugin_api::StateStorage> storage;
    const auto state_file = get_parameter("state_file").as_string();
    if (!state_file.empty()) {storage = std::make_shared<uran_platform_native::FileStateStorage>(state_file);}
    runtime_ = std::make_unique<uran_core_framework::UranRuntime>(
      std::make_shared<PluginlibLoader>(), platform_, storage);
    auto routes = YAML::Node(YAML::NodeType::Map);
    const auto route_file = get_parameter("route_config").as_string();
    if (!route_file.empty()) {routes = YAML::LoadFile(route_file)["routes"];}
    auto endpoint = [&routes](const std::string & name, const std::string & fallback) {
      return routes[name] ? routes[name].as<std::string>() : fallback;
    };
    snapshot_ = create_publisher<Text>(endpoint("state_snapshot", "/uran/runtime/state_snapshot"), 10);
    heartbeat_ = create_publisher<Text>(endpoint("heartbeat", "/uran/runtime/heartbeat"), 10);
    downlink_ = create_publisher<Text>(endpoint("downlink", "/uran/runtime/downlink"), 10);
    management_reply_ = create_publisher<Text>(endpoint("management_reply", "/uran/runtime/management/reply"), 10);
    runtime_->set_unhandled_handler([this](const auto & message) {
      const auto parsed = Json::parse(message.payload, nullptr, false);
      publish(downlink_, Json{{"message_type", message.message_type},
        {"payload", parsed.is_discarded() ? Json(message.payload) : parsed},
        {"correlation_id", message.correlation_id},
        {"source", {{"instance_id", message.source.instance_id},
        {"device_id", message.source.device_id}, {"component_id", message.source.component_id}}}}.dump());
    });
    const auto config_file = get_parameter("plugin_config").as_string();
    const auto started = config_file.empty() ? runtime_->start(YAML::Load("plugins: []")) : runtime_->start_file(config_file);
    if (!started.ok) {throw std::runtime_error(started.message);}
    uplink_ = create_subscription<Text>(endpoint("uplink", "/uran/runtime/uplink"), 10,
      [this](const Text::SharedPtr message) {
        guarded([&] {
          const auto body = Json::parse(message->data);
          check(runtime_->publish({body.at("message_type"), body.at("payload").dump(),
            body.value("correlation_id", ""), body.value("urgent", false)},
            body.value("preferred_protocol_instance", "")));
        });
      });
    state_write_ = create_subscription<Text>(endpoint("state_write", "/uran/runtime/state_write"), 10,
      [this](const Text::SharedPtr message) {
        guarded([&] {
          const auto body = Json::parse(message->data);
          check(runtime_->write_state(body.at("field"), body.at("value").dump(),
            "external.ros2", body.value("persistent", false)));
        });
      });
    management_ = create_subscription<Text>(endpoint("management", "/uran/runtime/management/request"), 10,
      [this](const Text::SharedPtr message) {
        Json reply;
        try {
          const auto body = Json::parse(message->data);
          const std::string operation = body.at("op");
          reply["correlation_id"] = body.value("correlation_id", "");
          uran_plugin_api::Status result;
          if (operation == "load") {result = runtime_->load(YAML::Load(body.at("plugin").dump()));}
          else if (operation == "activate") {result = runtime_->activate(body.at("instance_id"));}
          else if (operation == "deactivate") {result = runtime_->deactivate(body.at("instance_id"));}
          else if (operation == "unload") {result = runtime_->unload(body.at("instance_id"));}
          else if (operation == "primary") {result = runtime_->set_primary_protocol(body.at("instance_id"));}
          else if (operation == "status") {result = uran_plugin_api::Status::success(); reply["result"] = Json::parse(runtime_->diagnostics());}
          else {result = uran_plugin_api::Status::failure("unknown management operation");}
          reply["ok"] = result.ok;
          reply["message"] = result.message;
        } catch (const std::exception & exception) {reply["ok"] = false; reply["message"] = exception.what();}
        publish(management_reply_, reply.dump());
      });
    trigger_ = create_service<std_srvs::srv::Trigger>(endpoint("state_report", "/uran/runtime/state_report"),
      [this](const std_srvs::srv::Trigger::Request::SharedPtr,
        std_srvs::srv::Trigger::Response::SharedPtr response) {
        publish(snapshot_, runtime_->state_snapshot()); response->success = true;
        response->message = "local state snapshot published";
      });
    snapshot_timer_ = create_wall_timer(std::chrono::milliseconds(std::max<int64_t>(100,
      get_parameter("snapshot_period_ms").as_int())), [this] {publish(snapshot_, runtime_->state_snapshot());});
    heartbeat_timer_ = create_wall_timer(std::chrono::milliseconds(std::max<int64_t>(100,
      get_parameter("heartbeat_period_ms").as_int())), [this] {publish(heartbeat_, runtime_->diagnostics());});
  }
  ~Ros2RuntimeHost() override
  {
    if (runtime_) {runtime_->set_unhandled_handler(nullptr); runtime_->stop(); runtime_.reset();}
    platform_.reset();
  }
private:
  static void publish(const rclcpp::Publisher<Text>::SharedPtr & publisher, const std::string & data)
  {Text message; message.data = data; publisher->publish(message);}
  static void check(const uran_plugin_api::Status & result)
  {if (!result.ok) {throw std::runtime_error(result.message);}}
  template<typename Function> void guarded(Function function)
  {try {function();} catch (const std::exception & exception) {RCLCPP_WARN(get_logger(), "%s", exception.what());}}
  std::shared_ptr<Ros2Platform> platform_;
  std::unique_ptr<uran_core_framework::UranRuntime> runtime_;
  rclcpp::Publisher<Text>::SharedPtr snapshot_, heartbeat_, downlink_, management_reply_;
  rclcpp::Subscription<Text>::SharedPtr uplink_, state_write_, management_;
  rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr trigger_;
  rclcpp::TimerBase::SharedPtr snapshot_timer_, heartbeat_timer_;
};
}

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  int code = 0;
  try {
    auto node = std::make_shared<uran_ros2_adapter::Ros2RuntimeHost>();
    node->initialize();
    rclcpp::spin(node);
  } catch (const std::exception & exception) {
    RCLCPP_ERROR(rclcpp::get_logger("uran_runtime"), "%s", exception.what()); code = 1;
  }
  if (rclcpp::ok()) {rclcpp::shutdown();}
  return code;
}
