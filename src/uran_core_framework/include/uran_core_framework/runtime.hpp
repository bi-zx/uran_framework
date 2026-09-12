#pragma once

#include <memory>
#include <string>
#include <vector>
#include <yaml-cpp/yaml.h>
#include "uran_core_framework/capability_registry.hpp"
#include "uran_core_framework/message_router.hpp"
#include "uran_plugin_api/platform_services.hpp"
#include "uran_plugin_api/plugin_loader.hpp"
#include "uran_plugin_api/state_storage.hpp"

namespace uran_core_framework
{
/** Platform-independent runtime. The host must keep PlatformServices running
 * until this object is destroyed. All plugin calls execute on that serial executor.
 * Administrative mutations are rejected during callbacks invoked by Runtime.
 * Plugins must use PluginContext and may not synchronously manage their own lifecycle. */
class UranRuntime final
{
public:
  UranRuntime(std::shared_ptr<uran_plugin_api::PluginLoader>,
    std::shared_ptr<uran_plugin_api::PlatformServices>,
    std::shared_ptr<uran_plugin_api::StateStorage> storage = {});
  ~UranRuntime();
  UranRuntime(const UranRuntime &) = delete;
  UranRuntime & operator=(const UranRuntime &) = delete;

  Status start(const YAML::Node & configuration);
  Status start_file(const std::string & path);
  Status stop();
  Status load(const YAML::Node & specification);
  Status activate(const std::string & instance_id);
  Status deactivate(const std::string & instance_id);
  Status unload(const std::string & instance_id);
  Status set_primary_protocol(const std::string & instance_id);
  Status publish(const uran_plugin_api::UplinkEnvelope &, const std::string & preferred = {});
  Status dispatch(const uran_plugin_api::DownlinkEnvelope &);
  void set_unhandled_handler(MessageRouter::Handler);
  Status write_state(const std::string & field, const std::string & value_json,
    const std::string & source, bool persistent = false);
  std::string state_snapshot() const;
  std::string diagnostics() const;
  std::vector<CapabilityProvider> providers(const std::string & capability,
    const std::string & device = {}, const std::string & component = {}) const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
