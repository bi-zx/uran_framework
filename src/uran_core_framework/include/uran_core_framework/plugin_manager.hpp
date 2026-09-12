#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "uran_core_framework/types.hpp"
#include "uran_core_framework/visibility_control.hpp"
#include "uran_plugin_api/plugin.hpp"
#include "uran_plugin_api/plugin_context.hpp"
#include "uran_plugin_api/plugin_loader.hpp"

namespace uran_core_framework
{

class CapabilityRegistry;

struct URAN_CORE_FRAMEWORK_PUBLIC ActivePluginInstance
{
  PluginIdentity identity;
  PluginDescriptor descriptor;
  int priority{0};
  std::shared_ptr<uran_plugin_api::Plugin> instance;
};

/**
 * @brief 负责插件发现、受控上下文注入和生命周期事务。
 *
 * 插件方法均在管理锁外执行；同一插件的生命周期操作通过 transitioning 状态串行化。
 */
class URAN_CORE_FRAMEWORK_PUBLIC PluginManager final
{
public:
  explicit PluginManager(
    std::shared_ptr<uran_plugin_api::PluginLoader> loader,
    std::shared_ptr<CapabilityRegistry> capability_registry = nullptr);
  ~PluginManager();

  PluginManager(const PluginManager &) = delete;
  PluginManager & operator=(const PluginManager &) = delete;

  Status load(
    const PluginSpec & specification,
    const uran_plugin_api::PluginContext & context,
    const YAML::Node & config);
  Status activate(const std::string & instance_id);
  Status deactivate(const std::string & instance_id);
  Status unload(const std::string & instance_id);
  Status configure_and_load_all(
    const YAML::Node & plugins_node,
    const uran_plugin_api::PluginContext & context);

  std::vector<PluginRuntimeInfo> list() const;
  /** @brief 返回处于稳定激活状态、可以参与业务调用的插件实例。 */
  std::vector<ActivePluginInstance> active_instances() const;
  bool has(const std::string & instance_id) const;
  std::shared_ptr<uran_plugin_api::Plugin> instance(const std::string & instance_id) const;

private:
  struct Entry
  {
    PluginSpec specification;
    // 上下文必须比插件实例更晚析构，保证插件析构阶段仍可释放 Runtime 资源。
    std::shared_ptr<uran_plugin_api::PluginContext> context;
    std::shared_ptr<uran_plugin_api::Plugin> instance;
    PluginState state{PluginState::unloaded};
    bool transitioning{false};
    std::string error;
  };

  Entry * find_entry(const std::string & instance_id);
  const Entry * find_entry(const std::string & instance_id) const;
  static bool valid_identity_segment(const std::string & value);

  std::shared_ptr<CapabilityRegistry> capability_registry_;
  std::shared_ptr<uran_plugin_api::PluginLoader> loader_;
  std::mutex loader_mutex_;
  mutable std::mutex mutex_;
  std::map<std::string, Entry> entries_;
};

}
