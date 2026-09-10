#pragma once

#include <functional>
#include <string>

#include "uran_plugin_api/plugin.hpp"
#include "uran_plugin_api/visibility_control.hpp"

namespace uran_plugin_api
{

struct UplinkEnvelope
{
  std::string message_type;
  std::string payload;
  std::string correlation_id;
  bool urgent{false};
};

struct DownlinkEnvelope
{
  std::string message_type;
  std::string payload;
  std::string correlation_id;
};

using DownlinkCallback = std::function<void(const DownlinkEnvelope &)>;

/** @brief 外部通信协议的传输边界。 */
class URAN_PLUGIN_API_PUBLIC ProtocolPlugin : public Plugin
{
public:
  ~ProtocolPlugin() override = default;

  virtual Status connect() = 0;
  virtual Status disconnect() = 0;
  virtual Status publish(const UplinkEnvelope & message) = 0;
  virtual void set_downlink_callback(DownlinkCallback callback) = 0;
};

}
