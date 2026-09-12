#include "uran_core_framework/scoped_platform_services.hpp"
#include <algorithm>
#include <stdexcept>

namespace uran_core_framework
{
ScopedPlatformServices::ScopedPlatformServices(
  std::shared_ptr<PlatformServices> upstream, std::string owner, bool enabled)
: upstream_(std::move(upstream)), owner_(std::move(owner)), enabled_(enabled)
{
  if (!upstream_) {throw std::invalid_argument("platform is required");}
}
ScopedPlatformServices::~ScopedPlatformServices() {revoke();}
bool ScopedPlatformServices::enabled() const
{std::lock_guard<std::mutex> lock(mutex_); return enabled_;}
void ScopedPlatformServices::enable()
{
  std::lock_guard<std::mutex> lock(mutex_);
  enabled_ = true;
}
void ScopedPlatformServices::revoke() noexcept
{
  std::vector<std::weak_ptr<uran_plugin_api::Cancellation>> tasks;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_ = false;
    tasks.swap(tasks_);
  }
  for (const auto & task : tasks) {if (const auto live = task.lock()) {live->cancel();}}
}
void ScopedPlatformServices::log(
  uran_plugin_api::LogLevel level, const std::string &, const std::string & text)
{upstream_->log(level, owner_, text);}
std::chrono::system_clock::time_point ScopedPlatformServices::system_now() const
{return upstream_->system_now();}
std::chrono::steady_clock::time_point ScopedPlatformServices::steady_now() const
{return upstream_->steady_now();}
std::vector<std::string> ScopedPlatformServices::interfaces() const
{return upstream_->interfaces();}
bool ScopedPlatformServices::on_executor_thread() const noexcept
{return upstream_->on_executor_thread();}
std::shared_ptr<uran_plugin_api::Cancellation> ScopedPlatformServices::post(
  std::function<void()> callback)
{return schedule(std::chrono::milliseconds(0), false, std::move(callback));}
std::shared_ptr<uran_plugin_api::Cancellation> ScopedPlatformServices::schedule(
  std::chrono::milliseconds delay, bool repeat, std::function<void()> callback)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (!enabled_) {throw std::runtime_error("instance scheduling has been revoked");}
  tasks_.erase(std::remove_if(tasks_.begin(), tasks_.end(),
    [](const auto & task) {return task.expired();}), tasks_.end());
  auto task = upstream_->schedule(delay, repeat, std::move(callback));
  tasks_.push_back(task);
  return task;
}
}
