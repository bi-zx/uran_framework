#include <exception>
#include <memory>

#include <rclcpp/rclcpp.hpp>

#include "uran_core_framework/runtime_node.hpp"

int main(int argc, char * argv[])
{
  rclcpp::init(argc, argv);

  int exit_code = 0;
  try {
    auto node = std::make_shared<uran_core_framework::RuntimeNode>();
    const auto initialized = node->initialize_plugins();
    if (!initialized.ok) {
      RCLCPP_FATAL(node->get_logger(), "Runtime 初始化失败: %s", initialized.message.c_str());
      rclcpp::shutdown();
      return 1;
    }

    rclcpp::executors::MultiThreadedExecutor executor;
    executor.add_node(node);
    executor.spin();
    executor.remove_node(node);
  } catch (const std::exception & exception) {
    RCLCPP_FATAL(rclcpp::get_logger("uran_runtime"), "Runtime 异常退出: %s", exception.what());
    exit_code = 1;
  } catch (...) {
    RCLCPP_FATAL(rclcpp::get_logger("uran_runtime"), "Runtime 因未知异常退出");
    exit_code = 1;
  }

  if (rclcpp::ok()) {
    rclcpp::shutdown();
  }
  return exit_code;
}
