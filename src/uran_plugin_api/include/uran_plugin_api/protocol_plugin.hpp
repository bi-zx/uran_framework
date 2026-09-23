#pragma once

#include <functional>
#include <string>

#include "uran_plugin_api/plugin.hpp"
#include "uran_plugin_api/visibility_control.hpp"

namespace uran_plugin_api
{

struct MessageTarget
{
  std::string device_id;
  std::string component_id;
  std::string instance_id;
  std::string capability_id;
};

struct UplinkEnvelope
{
  std::string message_type;
  std::string payload;
  std::string correlation_id;
  bool urgent{false};
  PluginIdentity source;
};

struct DownlinkEnvelope
{
  std::string message_type;
  std::string payload;
  std::string correlation_id;
  // Filled by the gateway, never trusted from the transport payload.
  PluginIdentity source;
  MessageTarget target;
  std::uint64_t source_generation{0};
};

using DownlinkCallback = std::function<void(const DownlinkEnvelope &)>;

/** @brief 外部通信协议的传输边界。 */
class URAN_PLUGIN_API_PUBLIC ProtocolPlugin : public virtual Plugin
{
public:
  ~ProtocolPlugin() override = default;

  virtual Status connect() = 0;
  virtual Status disconnect() = 0;
  virtual Status publish(const UplinkEnvelope & message) = 0;
  virtual void set_downlink_callback(DownlinkCallback callback) = 0;
};

}
