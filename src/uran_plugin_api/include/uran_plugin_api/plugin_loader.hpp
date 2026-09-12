#pragma once

#include <memory>
#include <string>
#include <vector>

#include "uran_plugin_api/plugin.hpp"

namespace uran_plugin_api
{

inline constexpr const char * kPluginApiVersion = "2.0";
inline constexpr unsigned kNativePluginAbiVersion = 2;

/** Objects must retain their loader/library until their destructor returns. */
class PluginLoader
{
public:
  virtual ~PluginLoader() = default;
  virtual std::vector<std::string> available_classes() const = 0;
  virtual std::shared_ptr<Plugin> create(const std::string & class_type) = 0;
};

/** C entry point; the C++ interface still requires a compatible toolchain/ABI. */
struct NativePluginExport
{
  unsigned abi_version;
  const char * class_type;
  Plugin * (*create)();
  void (*destroy)(Plugin *) noexcept;
};
using NativePluginEntry = const NativePluginExport * (*)();

}
