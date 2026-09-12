#pragma once

#include <map>
#include <string>
#include <functional>

#include "uran_plugin_api/types.hpp"
#include "uran_plugin_api/visibility_control.hpp"

namespace uran_plugin_api
{

struct UplinkEnvelope;
class MessageAccess
{
public:
  virtual ~MessageAccess() = default;
  virtual Status publish(const UplinkEnvelope &, const std::string & preferred = {}) = 0;
};

struct CapabilityProvider
{
  std::string capability_id;
  PluginIdentity identity;
  int priority{0};
};
class CapabilityAccess
{
public:
  virtual ~CapabilityAccess() = default;
  virtual std::vector<CapabilityProvider> providers(const std::string & capability_id,
    const std::string & device_id = {}, const std::string & component_id = {}) const = 0;
};

/** @brief 插件可使用的状态访问接口。 */
class URAN_PLUGIN_API_PUBLIC StateAccess
{
public:
  virtual ~StateAccess() = default;

  virtual Status register_field(const StateFieldSpec & specification) = 0;
  virtual Status unregister_field(
    const std::string & field_name,
    const std::string & source) = 0;
  virtual Status set(
    const std::string & field_name,
    const std::string & value_json,
    const std::string & source,
    bool persistent = false) = 0;
  virtual bool get(const std::string & field_name, StateValue & value) const = 0;
  virtual std::map<std::string, StateValue> snapshot() const = 0;
  virtual std::string snapshot_json() const = 0;
};

/** @brief 插件可使用的路由查询接口。 */
class URAN_PLUGIN_API_PUBLIC RouteResolver
{
public:
  virtual ~RouteResolver() = default;
  virtual bool resolve(const std::string & route_name, std::string & route) const = 0;
};

}
