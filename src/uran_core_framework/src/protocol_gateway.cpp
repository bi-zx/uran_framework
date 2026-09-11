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
      protocols.push_back(item.second.protocol);
    }
    protocols_.clear();
    primary_instance_id_.clear();
    primary_explicit_ = false;
    downlink_handler_ = nullptr;
  }

  // 解除插件中的回调，避免网关销毁后仍收到异步下行消息。
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
  const PluginIdentity & identity,
  std::shared_ptr<uran_plugin_api::ProtocolPlugin> protocol,
  const int priority)
{
  if (identity.instance_id.empty() || identity.device_id.empty() ||
    identity.component_id.empty())
  {
    return Status::failure("协议实例的实例、设备和组件标识不能为空");
  }
  if (!protocol) {
    return Status::failure("协议插件实例不能为空: " + identity.instance_id);
  }
  if (weak_from_this().expired()) {
    return Status::failure("ProtocolGateway 必须由 shared_ptr 管理");
  }

  uran_plugin_api::PluginDescriptor descriptor;
  try {
    descriptor = protocol->descriptor();
  } catch (const std::exception & exception) {
    return Status::failure(std::string("读取协议插件实现描述失败: ") + exception.what());
  } catch (...) {
    return Status::failure("读取协议插件实现描述时发生未知异常");
  }
  if (descriptor.plugin_type != "protocol") {
    return Status::failure("插件类型不是 protocol: " + identity.instance_id);
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (protocols_.find(identity.instance_id) != protocols_.end()) {
      return Status::failure("协议实例已经注册: " + identity.instance_id);
    }
    protocols_.emplace(
      identity.instance_id, Entry{identity, protocol, priority});
    if (!primary_explicit_) {
      primary_instance_id_.clear();
    }
    select_default_primary_locked();
  }

  try {
    const std::weak_ptr<ProtocolGateway> weak_self = weak_from_this();
    protocol->set_downlink_callback(
      [weak_self, identity](const uran_plugin_api::DownlinkEnvelope & message) {
        if (const auto self = weak_self.lock()) {
          self->dispatch_downlink(identity, message);
        }
      });
  } catch (const std::exception & exception) {
    std::lock_guard<std::mutex> lock(mutex_);
    protocols_.erase(identity.instance_id);
    if (primary_instance_id_ == identity.instance_id) {
      primary_instance_id_.clear();
      primary_explicit_ = false;
    }
    select_default_primary_locked();
    return Status::failure(std::string("注册协议下行回调失败: ") + exception.what());
  } catch (...) {
    std::lock_guard<std::mutex> lock(mutex_);
    protocols_.erase(identity.instance_id);
    if (primary_instance_id_ == identity.instance_id) {
      primary_instance_id_.clear();
      primary_explicit_ = false;
    }
    select_default_primary_locked();
    return Status::failure("注册协议下行回调时发生未知异常");
  }
  return Status::success("协议实例已注册: " + identity.instance_id);
}

Status ProtocolGateway::unregister_protocol(const std::string & instance_id)
{
  std::shared_ptr<uran_plugin_api::ProtocolPlugin> protocol;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto iterator = protocols_.find(instance_id);
    if (iterator == protocols_.end()) {
      return Status::failure("协议实例不存在: " + instance_id);
    }
    protocol = iterator->second.protocol;
    protocols_.erase(iterator);
    if (primary_instance_id_ == instance_id) {
      primary_instance_id_.clear();
      primary_explicit_ = false;
    }
    select_default_primary_locked();
  }

  try {
    protocol->set_downlink_callback(nullptr);
  } catch (const std::exception & exception) {
    return Status::failure(
      "协议实例已移除，但解除下行回调失败: " + std::string(exception.what()));
  } catch (...) {
    return Status::failure("协议实例已移除，但解除下行回调时发生未知异常");
  }
  return Status::success("协议实例已移除: " + instance_id);
}

Status ProtocolGateway::set_primary_instance(const std::string & instance_id)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (protocols_.find(instance_id) == protocols_.end()) {
    return Status::failure("主协议实例尚未注册: " + instance_id);
  }
  primary_instance_id_ = instance_id;
  primary_explicit_ = true;
  return Status::success("主协议实例已切换: " + instance_id);
}

void ProtocolGateway::set_downlink_handler(DownlinkHandler handler)
{
  std::lock_guard<std::mutex> lock(mutex_);
  downlink_handler_ = std::move(handler);
}

Status ProtocolGateway::publish(
  const uran_plugin_api::UplinkEnvelope & message,
  const std::string & preferred_instance)
{
  if (message.message_type.empty()) {
    return Status::failure("上行消息类型不能为空");
  }

  std::vector<Entry> ordered;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> selected_ids;
    const auto append_instance = [this, &ordered, &selected_ids](const std::string & instance_id) {
        if (instance_id.empty() ||
          std::find(selected_ids.begin(), selected_ids.end(), instance_id) != selected_ids.end())
        {
          return;
        }
        const auto iterator = protocols_.find(instance_id);
        if (iterator != protocols_.end()) {
          selected_ids.push_back(instance_id);
          ordered.push_back(iterator->second);
        }
      };

    append_instance(preferred_instance);
    append_instance(primary_instance_id_);

    std::vector<Entry> remaining;
    for (const auto & item : protocols_) {
      if (std::find(selected_ids.begin(), selected_ids.end(), item.first) == selected_ids.end()) {
        remaining.push_back(item.second);
      }
    }
    std::stable_sort(
      remaining.begin(), remaining.end(),
      [](const Entry & left, const Entry & right) {
        if (left.priority != right.priority) {
          return left.priority > right.priority;
        }
        return left.identity.instance_id < right.identity.instance_id;
      });
    ordered.insert(ordered.end(), remaining.begin(), remaining.end());
  }

  if (ordered.empty()) {
    return Status::failure("没有可用的协议实例");
  }

  std::string failures;
  for (const auto & entry : ordered) {
    Status result;
    try {
      result = entry.protocol->publish(message);
    } catch (const std::exception & exception) {
      result = Status::failure(exception.what());
    } catch (...) {
      result = Status::failure("未知异常");
    }
    if (result.ok) {
      return Status::success("消息已通过协议实例发送: " + entry.identity.instance_id);
    }
    if (!failures.empty()) {
      failures += "; ";
    }
    failures += entry.identity.instance_id + "=" + result.message;
  }
  return Status::failure("所有协议实例发送均失败: " + failures);
}

std::vector<std::string> ProtocolGateway::protocol_instance_ids() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> result;
  result.reserve(protocols_.size());
  for (const auto & item : protocols_) {
    result.push_back(item.first);
  }
  return result;
}

std::string ProtocolGateway::primary_instance_id() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return primary_instance_id_;
}

void ProtocolGateway::select_default_primary_locked()
{
  if (!primary_instance_id_.empty() || protocols_.empty()) {
    return;
  }
  const auto selected = std::max_element(
    protocols_.begin(), protocols_.end(),
    [](const auto & left, const auto & right) {
      if (left.second.priority != right.second.priority) {
        return left.second.priority < right.second.priority;
      }
      return left.first > right.first;
    });
  primary_instance_id_ = selected->first;
}

void ProtocolGateway::dispatch_downlink(
  const PluginIdentity & identity,
  const uran_plugin_api::DownlinkEnvelope & message) const
{
  DownlinkHandler handler;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto protocol_iterator = protocols_.find(identity.instance_id);
    if (protocol_iterator == protocols_.end() ||
      protocol_iterator->second.identity != identity)
    {
      return;
    }
    handler = downlink_handler_;
  }
  if (handler) {
    try {
      handler(identity, message);
    } catch (...) {
      // 下行回调可能来自网络线程，异常不得穿透到第三方网络库。
    }
  }
}

}
