#pragma once
#include "uran_plugin_api/protocol_plugin.hpp"

namespace uran_plugin_api
{
/** Optional generic extension point. Domain semantics belong to implementations. */
class MessagePlugin : public Plugin
{
public:
  virtual std::vector<std::string> message_types() const = 0;
  virtual Status handle(const DownlinkEnvelope & message) = 0;
};
}
