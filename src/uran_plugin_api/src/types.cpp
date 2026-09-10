#include "uran_plugin_api/types.hpp"

namespace uran_plugin_api
{

const char * to_string(const PluginState state) noexcept
{
  switch (state) {
    case PluginState::unloaded:
      return "unloaded";
    case PluginState::loaded:
      return "loaded";
    case PluginState::configured:
      return "configured";
    case PluginState::active:
      return "active";
    case PluginState::inactive:
      return "inactive";
    case PluginState::error:
      return "error";
    default:
      return "unknown";
  }
}

}
