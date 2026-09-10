#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "uran_core_framework/types.hpp"
#include "uran_core_framework/visibility_control.hpp"
#include "uran_plugin_api/protocol_plugin.hpp"

namespace uran_core_framework
{

/**
 * @brief 管理协议插件并提供统一上行入口。
 *
 * 网关只处理协议选择和回退，不理解领域消息内容。
 */
class URAN_CORE_FRAMEWORK_PUBLIC ProtocolGateway final
  : public std::enable_shared_from_this<ProtocolGateway>
{
public:
  using DownlinkHandler = std::function<void(const uran_plugin_api::DownlinkEnvelope &)>;

  ProtocolGateway() = default;
  ~ProtocolGateway();

  ProtocolGateway(const ProtocolGateway &) = delete;
  ProtocolGateway & operator=(const ProtocolGateway &) = delete;

  Status register_protocol(
    const std::string & protocol_id,
    std::shared_ptr<uran_plugin_api::ProtocolPlugin> protocol);
  Status unregister_protocol(const std::string & protocol_id);
  Status set_primary(const std::string & protocol_id);
  void set_downlink_handler(DownlinkHandler handler);

  Status publish(
    const uran_plugin_api::UplinkEnvelope & message,
    const std::string & preferred_protocol = {});
  std::vector<std::string> protocol_ids() const;
  std::string primary_protocol() const;

private:
  void dispatch_downlink(const uran_plugin_api::DownlinkEnvelope & message) const;

  mutable std::mutex mutex_;
  std::map<std::string, std::shared_ptr<uran_plugin_api::ProtocolPlugin>> protocols_;
  std::string primary_protocol_;
  DownlinkHandler downlink_handler_;
};

} 
