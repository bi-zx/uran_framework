#pragma once

#include "uran_plugin_api/types.hpp"

namespace uran_core_framework
{

// 这些别名仅供 Runtime 内部使用；第三方插件应直接包含 uran_plugin_api。
using uran_plugin_api::PluginCapabilities;
using uran_plugin_api::PluginDescriptor;
using uran_plugin_api::PluginHealth;
using uran_plugin_api::PluginRuntimeInfo;
using uran_plugin_api::PluginSpec;
using uran_plugin_api::PluginState;
using uran_plugin_api::StateFieldSpec;
using uran_plugin_api::StateValue;
using uran_plugin_api::Status;
using uran_plugin_api::to_string;

}
