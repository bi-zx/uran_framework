#include "uran_plugin_api/plugin_context.hpp"

#include <stdexcept>
#include <utility>

namespace uran_plugin_api
{

PluginContext::PluginContext(
  std::shared_ptr<PlatformServices> platform,
  std::shared_ptr<StateAccess> state_access,
  std::shared_ptr<RouteResolver> route_resolver,
  PluginIdentity identity,
  std::string route_scope,
  std::shared_ptr<MessageAccess> messages,
  std::shared_ptr<CapabilityAccess> capabilities)
: PluginContext(std::move(platform), std::move(state_access), std::move(route_resolver),
    std::move(identity), std::move(route_scope), std::move(messages),
    std::move(capabilities), {})
{}

PluginContext::PluginContext(
  std::shared_ptr<PlatformServices> platform,
  std::shared_ptr<StateAccess> state_access,
  std::shared_ptr<RouteResolver> route_resolver,
  PluginIdentity identity,
  std::string route_scope,
  std::shared_ptr<MessageAccess> messages,
  std::shared_ptr<CapabilityAccess> capabilities,
  std::shared_ptr<LocalMessageAccess> local_messages,
  std::shared_ptr<StateObserverAccess> state_observer)
: platform_(std::move(platform)),
  state_access_(std::move(state_access)),
  route_resolver_(std::move(route_resolver)),
  identity_(std::move(identity)),
  route_scope_(std::move(route_scope)),
  messages_(std::move(messages)), capabilities_(std::move(capabilities)),
  local_messages_(std::move(local_messages)), state_observer_(std::move(state_observer))
{
  if (!platform_) {
    throw std::invalid_argument("PluginContext 的平台服务不能为空");
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

std::shared_ptr<PlatformServices> PluginContext::platform() const noexcept
{
  return platform_;
}

std::shared_ptr<MessageAccess> PluginContext::messages() const noexcept {return messages_;}
std::shared_ptr<CapabilityAccess> PluginContext::capabilities() const noexcept {return capabilities_;}
std::shared_ptr<LocalMessageAccess> PluginContext::local_messages() const noexcept {return local_messages_;}
std::shared_ptr<StateObserverAccess> PluginContext::state_observer() const noexcept {return state_observer_;}

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

const std::string & PluginContext::route_scope() const noexcept
{
  return route_scope_;
}

}
