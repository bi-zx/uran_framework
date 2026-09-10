#pragma once

#include <yaml-cpp/yaml.h>

#include "uran_plugin_api/plugin_context.hpp"
#include "uran_plugin_api/types.hpp"
#include "uran_plugin_api/visibility_control.hpp"

namespace uran_plugin_api
{

/**
 * @brief URAN 进程内插件的公共基类。
 *
 * 基类只定义元数据、生命周期、健康状态和能力，不包含具体领域操作。
 */
class URAN_PLUGIN_API_PUBLIC Plugin
{
public:
  virtual ~Plugin() = default;

  virtual PluginDescriptor descriptor() const = 0;
  virtual Status configure(
    const PluginContext & context,
    const YAML::Node & config) = 0;
  virtual Status activate() = 0;
  virtual Status deactivate() = 0;
  virtual Status cleanup() = 0;
  virtual PluginHealth health() const = 0;
  virtual PluginCapabilities capabilities() const = 0;
};

}
