#include "uran_platform_native/native_plugin_loader.hpp"

#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace uran_platform_native
{
uran_plugin_api::Status NativePluginLoader::add_manifest(const std::string & path, bool replace)
{
  try {
    std::ifstream input(path);
    if (!input) {throw std::runtime_error("cannot open plugin manifest: " + path);}
    const auto manifest = nlohmann::json::parse(input);
    if (manifest.at("schema_version") != 1 ||
      manifest.at("api_version") != uran_plugin_api::kPluginApiVersion)
    {throw std::runtime_error("incompatible plugin manifest/API version");}
    const auto entries = manifest.at("plugins");
    if (!entries.is_array() || entries.empty()) {throw std::runtime_error("empty plugin manifest");}
    std::map<std::string, std::string> candidate;
    for (const auto & entry : entries) {
      const auto name = entry.at("class_type").get<std::string>();
      const auto library = std::filesystem::canonical(
        std::filesystem::path(path).parent_path() / entry.at("library").get<std::string>());
      if (name.empty() || !std::filesystem::is_regular_file(library) ||
        !candidate.emplace(name, library.string()).second)
      {throw std::runtime_error("invalid or duplicate plugin class: " + name);}
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto & item : candidate) {
      const auto existing = libraries_.find(item.first);
      if (!replace && existing != libraries_.end() && existing->second != item.second) {
        throw std::runtime_error("plugin class already mapped: " + item.first);
      }
    }
    for (const auto & item : candidate) {libraries_[item.first] = item.second;}
    return uran_plugin_api::Status::success();
  } catch (const std::exception & exception) {
    return uran_plugin_api::Status::failure(exception.what());
  }
}

std::vector<std::string> NativePluginLoader::available_classes() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> result;
  for (const auto & item : libraries_) {result.push_back(item.first);}
  return result;
}

std::shared_ptr<uran_plugin_api::Plugin> NativePluginLoader::create(const std::string & class_type)
{
  std::string path;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = libraries_.find(class_type);
    if (it == libraries_.end()) {throw std::runtime_error("plugin class not found: " + class_type);}
    path = it->second;
  }
  const auto raw_library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!raw_library) {throw std::runtime_error(std::string("dlopen: ") + dlerror());}
  auto library = std::shared_ptr<void>(raw_library, [](void * handle) {dlclose(handle);});
  dlerror();
  auto entry = reinterpret_cast<uran_plugin_api::NativePluginEntry>(
    dlsym(library.get(), "uran_plugin_export_v3"));
  const char * error = dlerror();
  if (error || !entry) {throw std::runtime_error("missing uran_plugin_export_v3: " + path);}
  const auto exported = entry();
  if (!exported || exported->abi_version != uran_plugin_api::kNativePluginAbiVersion ||
    !exported->class_type || class_type != exported->class_type ||
    !exported->create || !exported->destroy)
  {throw std::runtime_error("invalid native plugin export: " + class_type);}
  const auto destroy = exported->destroy;
  auto instance = exported->create();
  if (!instance) {throw std::runtime_error("plugin factory returned null: " + class_type);}
  return {instance, [library, destroy](uran_plugin_api::Plugin * plugin) {destroy(plugin);}};
}
}
