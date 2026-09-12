#pragma once
#include <map>
#include <mutex>
#include "uran_plugin_api/plugin_loader.hpp"

namespace uran_platform_native
{
/** POSIX dynamic libraries are isolated here, never in the core or public API. */
class NativePluginLoader final : public uran_plugin_api::PluginLoader
{
public:
  uran_plugin_api::Status add_manifest(const std::string & path, bool replace = false);
  std::vector<std::string> available_classes() const override;
  std::shared_ptr<uran_plugin_api::Plugin> create(const std::string & class_type) override;
private:
  mutable std::mutex mutex_;
  std::map<std::string, std::string> libraries_;
};
}
