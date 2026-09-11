#pragma once

#include <memory>
#include <string>

#include <rclcpp/rclcpp.hpp>

#include "uran_plugin_api/runtime_access.hpp"
#include "uran_plugin_api/types.hpp"
#include "uran_plugin_api/visibility_control.hpp"

namespace uran_plugin_api
{

/**
 * @brief Runtime 向插件开放的受控上下文。
 *
 * 插件通过该对象使用公共运行能力，而不是访问 Runtime 内部管理器。
 */
class URAN_PLUGIN_API_PUBLIC PluginContext final
{
public:
  PluginContext(
    rclcpp::Node::SharedPtr node,
    std::shared_ptr<StateAccess> state_access,
    std::shared_ptr<RouteResolver> route_resolver,
    PluginIdentity identity,
    std::string instance_namespace);

  rclcpp::Node::SharedPtr node() const noexcept;
  std::shared_ptr<StateAccess> state_access() const noexcept;
  std::shared_ptr<RouteResolver> route_resolver() const noexcept;
  const PluginIdentity & identity() const noexcept;
  const std::string & instance_id() const noexcept;
  const std::string & device_id() const noexcept;
  const std::string & component_id() const noexcept;
  const std::string & instance_namespace() const noexcept;

private:
  std::weak_ptr<rclcpp::Node> node_;
  std::shared_ptr<StateAccess> state_access_;
  std::shared_ptr<RouteResolver> route_resolver_;
  PluginIdentity identity_;
  std::string instance_namespace_;
};

}
