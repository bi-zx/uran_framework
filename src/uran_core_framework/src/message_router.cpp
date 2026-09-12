#include "uran_core_framework/message_router.hpp"
#include <algorithm>
#include <exception>

namespace uran_core_framework
{
Status MessageRouter::register_handler(
  const std::string & type, Handler handler, std::size_t & id)
{
  if (!handler) {id = 0; return Status::failure("empty handler");}
  return register_handler(MessageRoute{type, {}, {}, 0},
    [handler](const auto & message) {handler(message); return Status::success();}, id);
}
Status MessageRouter::register_handler(
  const MessageRoute & route, ResultHandler handler, std::size_t & id)
{
  id = 0;
  if (route.message_type.empty() || !handler) {return Status::failure("invalid message route");}
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto & item : handlers_) {
    if (item.second.route.message_type == route.message_type &&
      item.second.route.provider == route.provider)
    {return Status::failure("duplicate message route");}
  }
  id = next_id_++;
  handlers_.emplace(id, Entry{route, std::move(handler)});
  return Status::success();
}
void MessageRouter::unregister_handler(std::size_t id)
{std::lock_guard<std::mutex> lock(mutex_); handlers_.erase(id);}
void MessageRouter::set_unhandled_handler(Handler handler)
{std::lock_guard<std::mutex> lock(mutex_); unhandled_handler_ = std::move(handler);}
Status MessageRouter::route(const uran_plugin_api::DownlinkEnvelope & message) const
{
  if (message.message_type.empty()) {return Status::failure("message type is required");}
  ResultHandler selected;
  Handler fallback;
  const auto & target = message.target;
  const bool targeted = !target.device_id.empty() || !target.component_id.empty() ||
    !target.instance_id.empty() || !target.capability_id.empty();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const Entry * best = nullptr;
    for (const auto & item : handlers_) {
      const auto & entry = item.second;
      const auto & route = entry.route;
      if (route.message_type != message.message_type) {continue;}
      if (!targeted && !route.provider.instance_id.empty()) {continue;}
      if (targeted && route.provider.instance_id.empty()) {continue;}
      if (!target.device_id.empty() && target.device_id != route.provider.device_id) {continue;}
      if (!target.component_id.empty() && target.component_id != route.provider.component_id) {continue;}
      if (!target.instance_id.empty() && target.instance_id != route.provider.instance_id) {continue;}
      if (!target.capability_id.empty() && std::find(route.capabilities.begin(),
        route.capabilities.end(), target.capability_id) == route.capabilities.end()) {continue;}
      if (!best || route.priority > best->route.priority ||
        (route.priority == best->route.priority &&
        route.provider.instance_id < best->route.provider.instance_id)) {best = &entry;}
    }
    if (best) {selected = best->handler;}
    fallback = unhandled_handler_;
  }
  try {
    if (selected) {return selected(message);}
    if (fallback) {fallback(message); return Status::success("unhandled message forwarded");}
    return Status::failure("no matching message handler");
  } catch (const std::exception & exception) {return Status::failure(exception.what());}
  catch (...) {return Status::failure("message handler threw an unknown exception");}
}
}
