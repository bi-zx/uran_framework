#include "uran_core_framework/protocol_gateway.hpp"

#include <algorithm>
#include <exception>
#include <utility>

namespace uran_core_framework
{

ProtocolGateway::~ProtocolGateway()
{
  std::vector<std::shared_ptr<uran_plugin_api::ProtocolPlugin>> protocols;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    protocols.reserve(protocols_.size());
    for (const auto & item : protocols_) {
      protocols.push_back(item.second);
    }
    protocols_.clear();
    downlink_handler_ = nullptr;
  }

  // 解除插件中的回调，避免网关销毁后仍收到异步下行消息
  for (const auto & protocol : protocols) {
    if (protocol) {
      try {
        protocol->set_downlink_callback(nullptr);
      } catch (...) {
      }
    }
  }
}

Status ProtocolGateway::register_protocol(
  const std::string & protocol_id,
  std::shared_ptr<uran_plugin_api::ProtocolPlugin> protocol)
{
  if (protocol_id.empty()) {
    return Status::failure("协议 ID 不能为空");
  }
  if (!protocol) {
    return Status::failure("协议插件实例不能为空: " + protocol_id);
  }
  if (weak_from_this().expired()) {
    return Status::failure("ProtocolGateway 必须由 shared_ptr 管理");
  }

  uran_plugin_api::PluginDescriptor descriptor;
  try {
    descriptor = protocol->descriptor();
  } catch (const std::exception & exception) {
    return Status::failure(std::string("读取协议插件描述失败: ") + exception.what());
  } catch (...) {
    return Status::failure("读取协议插件描述时发生未知异常");
  }
  if (descriptor.plugin_id != protocol_id) {
    return Status::failure("协议 ID 与插件描述不一致: " + protocol_id);
  }
  if (descriptor.plugin_type != "protocol") {
    return Status::failure("插件类型不是 protocol: " + protocol_id);
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (protocols_.find(protocol_id) != protocols_.end()) {
      return Status::failure("协议已经注册: " + protocol_id);
    }
    protocols_.emplace(protocol_id, protocol);
    if (primary_protocol_.empty()) {
      primary_protocol_ = protocol_id;
    }
  }

  try {
    const std::weak_ptr<ProtocolGateway> weak_self = weak_from_this();
    protocol->set_downlink_callback(
      [weak_self](const uran_plugin_api::DownlinkEnvelope & message) {
        if (const auto self = weak_self.lock()) {
          self->dispatch_downlink(message);
        }
      });
  } catch (const std::exception & exception) {
    std::lock_guard<std::mutex> lock(mutex_);
    protocols_.erase(protocol_id);
    if (primary_protocol_ == protocol_id) {
      primary_protocol_ = protocols_.empty() ? std::string{} : protocols_.begin()->first;
    }
    return Status::failure(std::string("注册协议下行回调失败: ") + exception.what());
  } catch (...) {
    std::lock_guard<std::mutex> lock(mutex_);
    protocols_.erase(protocol_id);
    if (primary_protocol_ == protocol_id) {
      primary_protocol_ = protocols_.empty() ? std::string{} : protocols_.begin()->first;
    }
    return Status::failure("注册协议下行回调时发生未知异常");
  }
  return Status::success("协议已注册: " + protocol_id);
}

Status ProtocolGateway::unregister_protocol(const std::string & protocol_id)
{
  std::shared_ptr<uran_plugin_api::ProtocolPlugin> protocol;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto iterator = protocols_.find(protocol_id);
    if (iterator == protocols_.end()) {
      return Status::failure("协议不存在: " + protocol_id);
    }
    protocol = iterator->second;
    protocols_.erase(iterator);
    if (primary_protocol_ == protocol_id) {
      primary_protocol_ = protocols_.empty() ? std::string{} : protocols_.begin()->first;
    }
  }

  try {
    protocol->set_downlink_callback(nullptr);
  } catch (const std::exception & exception) {
    return Status::failure(
      "协议已移除，但解除下行回调失败: " + std::string(exception.what()));
  } catch (...) {
    return Status::failure("协议已移除，但解除下行回调时发生未知异常");
  }
  return Status::success("协议已移除: " + protocol_id);
}

Status ProtocolGateway::set_primary(const std::string & protocol_id)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (protocols_.find(protocol_id) == protocols_.end()) {
    return Status::failure("主协议尚未注册: " + protocol_id);
  }
  primary_protocol_ = protocol_id;
  return Status::success("主协议已切换: " + protocol_id);
}

void ProtocolGateway::set_downlink_handler(DownlinkHandler handler)
{
  std::lock_guard<std::mutex> lock(mutex_);
  downlink_handler_ = std::move(handler);
}

Status ProtocolGateway::publish(
  const uran_plugin_api::UplinkEnvelope & message,
  const std::string & preferred_protocol)
{
  if (message.message_type.empty()) {
    return Status::failure("上行消息类型不能为空");
  }

  std::vector<std::pair<std::string, std::shared_ptr<uran_plugin_api::ProtocolPlugin>>> ordered;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> order;
    if (!preferred_protocol.empty()) {
      order.push_back(preferred_protocol);
    }
    if (!primary_protocol_.empty() && primary_protocol_ != preferred_protocol) {
      order.push_back(primary_protocol_);
    }
    for (const auto & item : protocols_) {
      if (std::find(order.begin(), order.end(), item.first) == order.end()) {
        order.push_back(item.first);
      }
    }
    for (const auto & id : order) {
      const auto iterator = protocols_.find(id);
      if (iterator != protocols_.end()) {
        ordered.emplace_back(iterator->first, iterator->second);
      }
    }
  }

  if (ordered.empty()) {
    return Status::failure("没有可用的协议插件");
  }

  std::string failures;
  for (const auto & item : ordered) {
    Status result;
    try {
      result = item.second->publish(message);
    } catch (const std::exception & exception) {
      result = Status::failure(exception.what());
    } catch (...) {
      result = Status::failure("未知异常");
    }
    if (result.ok) {
      return Status::success("消息已通过协议发送: " + item.first);
    }
    if (!failures.empty()) {
      failures += "; ";
    }
    failures += item.first + "=" + result.message;
  }
  return Status::failure("所有协议发送均失败: " + failures);
}

std::vector<std::string> ProtocolGateway::protocol_ids() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> result;
  result.reserve(protocols_.size());
  for (const auto & item : protocols_) {
    result.push_back(item.first);
  }
  return result;
}

std::string ProtocolGateway::primary_protocol() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return primary_protocol_;
}

void ProtocolGateway::dispatch_downlink(
  const uran_plugin_api::DownlinkEnvelope & message) const
{
  DownlinkHandler handler;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    handler = downlink_handler_;
  }
  if (handler) {
    try {
      handler(message);
    } catch (...) {
      // 下行回调可能来自网络线程，异常不得穿透到第三方网络库
    }
  }
}

}
