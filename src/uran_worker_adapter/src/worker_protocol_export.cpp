#include "uran_worker_adapter/worker_process_plugin.hpp"
#include "uran_plugin_api/plugin_loader.hpp"

extern "C" URAN_PLUGIN_API_EXPORT const uran_plugin_api::NativePluginExport * uran_plugin_export_v3()
{
  static const uran_plugin_api::NativePluginExport exported{
    uran_plugin_api::kNativePluginAbiVersion,
    "uran_worker_adapter/WorkerProtocolPlugin",
    []() -> uran_plugin_api::Plugin * {return new uran_worker_adapter::WorkerProtocolPlugin;},
    [](uran_plugin_api::Plugin * plugin) noexcept {delete plugin;}};
  return &exported;
}
