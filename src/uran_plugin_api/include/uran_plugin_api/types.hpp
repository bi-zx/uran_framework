#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "uran_plugin_api/visibility_control.hpp"

namespace uran_plugin_api
{

/** @brief 跨模块操作使用的轻量结果。 */
struct Status
{
  bool ok{false};
  std::string message;

  static Status success(std::string message = {})
  {
    return Status{true, std::move(message)};
  }

  static Status failure(std::string message)
  {
    return Status{false, std::move(message)};
  }
};

enum class PluginState
{
  unloaded,
  loaded,
  configured,
  active,
  inactive,
  error
};

URAN_PLUGIN_API_PUBLIC const char * to_string(PluginState state) noexcept;

struct PluginHealth
{
  enum class Level
  {
    unknown,
    starting,
    healthy,
    degraded,
    error,
    stopped
  };

  Level level{Level::unknown};
  std::string message;
  std::chrono::system_clock::time_point checked_at{};
  std::uint64_t error_count{0};
};

struct PluginCapabilities
{
  std::vector<std::string> features;
  std::vector<std::string> required_interfaces;
  std::vector<std::string> provided_state_fields;
};

struct PluginDescriptor
{
  std::string plugin_id;
  std::string plugin_type;
  std::string plugin_version;
  std::string api_version;
  std::string class_type;
};

struct PluginSpec
{
  PluginDescriptor descriptor;
  bool enabled{true};
  int priority{0};
  std::string config_file;
};

struct StateFieldSpec
{
  std::string name;
  std::string type;
  std::string unit;
  std::string owner;
  bool persistent{false};
};

struct StateValue
{
  std::string value_json;
  std::string source;
  std::chrono::system_clock::time_point updated_at{};
  bool persistent{false};
};

struct PluginRuntimeInfo
{
  PluginDescriptor descriptor;
  PluginState state{PluginState::unloaded};
  PluginHealth health;
  PluginCapabilities capabilities;
  std::string error;
};

}
