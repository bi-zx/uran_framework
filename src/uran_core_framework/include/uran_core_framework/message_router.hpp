#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <string>

#include "uran_core_framework/types.hpp"
#include "uran_core_framework/visibility_control.hpp"
#include "uran_plugin_api/protocol_plugin.hpp"

namespace uran_core_framework
{

/** @brief 按消息类型分发协议下行消息。 */
class URAN_CORE_FRAMEWORK_PUBLIC MessageRouter final
{
public:
  using Handler = std::function<void(const uran_plugin_api::DownlinkEnvelope &)>;

  Status register_handler(
    const std::string & message_type,
    Handler handler,
    std::size_t & registration_id);
  void unregister_handler(std::size_t registration_id);
  void set_unhandled_handler(Handler handler);

  Status route(const uran_plugin_api::DownlinkEnvelope & message) const;

private:
  struct Entry
  {
    std::size_t id{0};
    Handler handler;
  };

  mutable std::mutex mutex_;
  std::map<std::string, Entry> handlers_;
  Handler unhandled_handler_;
  std::size_t next_registration_id_{1};
};

}
