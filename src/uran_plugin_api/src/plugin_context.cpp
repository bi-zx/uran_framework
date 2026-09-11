#include "uran_plugin_api/plugin_context.hpp"

#include <stdexcept>
#include <utility>

namespace uran_plugin_api
{

PluginContext::PluginContext(
  rclcpp::Node::SharedPtr node,
  std::shared_ptr<StateAccess> state_access,
  std::shared_ptr<RouteResolver> route_resolver,
  PluginIdentity identity,
  std::string instance_namespace)
: node_(std::move(node)),
  state_access_(std::move(state_access)),
  route_resolver_(std::move(route_resolver)),
  identity_(std::move(identity)),
  instance_namespace_(std::move(instance_namespace))
{
  if (!node_) {
    throw std::invalid_argument("PluginContext 的 node 不能为空");
  }
  if (!state_access_) {
    throw std::invalid_argument("PluginContext 的 state_access 不能为空");
  }
  if (!route_resolver_) {
    throw std::invalid_argument("PluginContext 的 route_resolver 不能为空");
  }
  if (identity_.instance_id.empty() || identity_.device_id.empty() ||
    identity_.component_id.empty())
  {
    throw std::invalid_argument("PluginContext 的实例、设备和组件标识不能为空");
  }
}

rclcpp::Node::SharedPtr PluginContext::node() const noexcept
{
  return node_.lock();
}

std::shared_ptr<StateAccess> PluginContext::state_access() const noexcept
{
  return state_access_;
}

std::shared_ptr<RouteResolver> PluginContext::route_resolver() const noexcept
{
  return route_resolver_;
}

const PluginIdentity & PluginContext::identity() const noexcept
{
  return identity_;
}

const std::string & PluginContext::instance_id() const noexcept
{
  return identity_.instance_id;
}

const std::string & PluginContext::device_id() const noexcept
{
  return identity_.device_id;
}

const std::string & PluginContext::component_id() const noexcept
{
  return identity_.component_id;
}

const std::string & PluginContext::instance_namespace() const noexcept
{
  return instance_namespace_;
}

}
