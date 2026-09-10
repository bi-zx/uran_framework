#include "uran_core_framework/message_router.hpp"

#include <exception>
#include <utility>

namespace uran_core_framework
{

Status MessageRouter::register_handler(
  const std::string & message_type,
  Handler handler,
  std::size_t & registration_id)
{
  registration_id = 0U;
  if (message_type.empty()) {
    return Status::failure("消息类型不能为空");
  }
  if (!handler) {
    return Status::failure("消息处理器不能为空: " + message_type);
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (handlers_.find(message_type) != handlers_.end()) {
    return Status::failure("消息类型已经注册处理器: " + message_type);
  }
  registration_id = next_registration_id_++;
  handlers_.emplace(message_type, Entry{registration_id, std::move(handler)});
  return Status::success();
}

void MessageRouter::unregister_handler(const std::size_t registration_id)
{
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto iterator = handlers_.begin(); iterator != handlers_.end(); ++iterator) {
    if (iterator->second.id == registration_id) {
      handlers_.erase(iterator);
      return;
    }
  }
}

void MessageRouter::set_unhandled_handler(Handler handler)
{
  std::lock_guard<std::mutex> lock(mutex_);
  unhandled_handler_ = std::move(handler);
}

Status MessageRouter::route(const uran_plugin_api::DownlinkEnvelope & message) const
{
  if (message.message_type.empty()) {
    return Status::failure("下行消息类型不能为空");
  }

  Handler handler;
  bool matched = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto iterator = handlers_.find(message.message_type);
    if (iterator != handlers_.end()) {
      handler = iterator->second.handler;
      matched = true;
    } else {
      handler = unhandled_handler_;
    }
  }

  if (!handler) {
    return Status::failure("没有可处理该类型下行消息的处理器: " + message.message_type);
  }

  try {
    handler(message);
  } catch (const std::exception & exception) {
    return Status::failure(
      "下行消息处理器抛出异常: " + message.message_type + ", 原因: " + exception.what());
  } catch (...) {
    return Status::failure("下行消息处理器抛出未知异常: " + message.message_type);
  }

  return Status::success(
    matched ? "下行消息已路由" : "下行消息已交给未匹配处理器");
}

}
