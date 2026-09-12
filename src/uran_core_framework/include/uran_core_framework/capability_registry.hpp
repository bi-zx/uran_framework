#pragma once

#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "uran_core_framework/types.hpp"
#include "uran_plugin_api/runtime_access.hpp"
#include "uran_core_framework/visibility_control.hpp"

namespace uran_core_framework
{

/** @brief 一项语义能力及其实际运行提供者。 */
using uran_plugin_api::CapabilityProvider;

/**
 * @brief 按设备、组件和插件实例维护当前可用能力。
 *
 * 同一实例可以提供多项能力，同一能力也可以由多个实例提供。
 */
class URAN_CORE_FRAMEWORK_PUBLIC CapabilityRegistry final
  : public uran_plugin_api::CapabilityAccess
{
public:
  Status register_instance(
    const PluginIdentity & identity,
    const std::vector<std::string> & capability_ids,
    int priority = 0);
  Status unregister_instance(const std::string & instance_id);

  std::vector<CapabilityProvider> providers(
    const std::string & capability_id,
    const std::string & device_id = {},
    const std::string & component_id = {}) const override;
  std::vector<std::string> capabilities_for_instance(
    const std::string & instance_id) const;

  std::size_t capability_count() const;
  std::size_t provider_count() const;

private:
  static bool valid_identifier(const std::string & value);
  static bool valid_capability_id(const std::string & value);

  mutable std::mutex mutex_;
  std::map<std::string, PluginIdentity> identities_;
  std::map<std::string, std::map<std::string, CapabilityProvider>> providers_;
};

}
