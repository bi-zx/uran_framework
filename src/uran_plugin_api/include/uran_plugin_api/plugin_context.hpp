#pragma once

#include <memory>
#include <string>

#include "uran_plugin_api/platform_services.hpp"

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
    std::shared_ptr<PlatformServices> platform,
    std::shared_ptr<StateAccess> state_access,
    std::shared_ptr<RouteResolver> route_resolver,
    PluginIdentity identity,
    std::string route_scope,
    std::shared_ptr<MessageAccess> messages = {},
    std::shared_ptr<CapabilityAccess> capabilities = {});
  PluginContext(
    std::shared_ptr<PlatformServices> platform,
    std::shared_ptr<StateAccess> state_access,
    std::shared_ptr<RouteResolver> route_resolver,
    PluginIdentity identity,
    std::string route_scope,
    std::shared_ptr<MessageAccess> messages,
    std::shared_ptr<CapabilityAccess> capabilities,
    std::shared_ptr<LocalMessageAccess> local_messages,
    std::shared_ptr<StateObserverAccess> state_observer = {},
    std::shared_ptr<RuntimeSnapshotAccess> runtime_snapshot = {});

  std::shared_ptr<PlatformServices> platform() const noexcept;
  std::shared_ptr<StateAccess> state_access() const noexcept;
  std::shared_ptr<RouteResolver> route_resolver() const noexcept;
  const PluginIdentity & identity() const noexcept;
  const std::string & instance_id() const noexcept;
  const std::string & device_id() const noexcept;
  const std::string & component_id() const noexcept;
  const std::string & route_scope() const noexcept;
  std::shared_ptr<MessageAccess> messages() const noexcept;
  std::shared_ptr<CapabilityAccess> capabilities() const noexcept;
  std::shared_ptr<LocalMessageAccess> local_messages() const noexcept;
  std::shared_ptr<StateObserverAccess> state_observer() const noexcept;
  std::shared_ptr<RuntimeSnapshotAccess> runtime_snapshot() const noexcept;

private:
  std::shared_ptr<PlatformServices> platform_;
  std::shared_ptr<StateAccess> state_access_;
  std::shared_ptr<RouteResolver> route_resolver_;
  PluginIdentity identity_;
  std::string route_scope_;
  std::shared_ptr<MessageAccess> messages_;
  std::shared_ptr<CapabilityAccess> capabilities_;
  std::shared_ptr<LocalMessageAccess> local_messages_;
  std::shared_ptr<StateObserverAccess> state_observer_;
  std::shared_ptr<RuntimeSnapshotAccess> runtime_snapshot_;
};

}
