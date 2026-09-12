#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "uran_core_framework/types.hpp"
#include "uran_plugin_api/protocol_plugin.hpp"

namespace uran_core_framework
{
struct MessageRoute
{
  std::string message_type;
  PluginIdentity provider;
  std::vector<std::string> capabilities;
  int priority{0};
};

/** Synchronous semantic routing; Runtime invokes it only on its serial executor. */
class MessageRouter final
{
public:
  using Handler = std::function<void(const uran_plugin_api::DownlinkEnvelope &)>;
  using ResultHandler = std::function<Status(const uran_plugin_api::DownlinkEnvelope &)>;
  Status register_handler(const std::string &, Handler, std::size_t &);
  Status register_handler(const MessageRoute &, ResultHandler, std::size_t &);
  void unregister_handler(std::size_t);
  void set_unhandled_handler(Handler);
  Status route(const uran_plugin_api::DownlinkEnvelope &) const;
private:
  struct Entry {MessageRoute route; ResultHandler handler;};
  mutable std::mutex mutex_;
  std::map<std::size_t, Entry> handlers_;
  Handler unhandled_handler_;
  std::size_t next_id_{1};
};
}
