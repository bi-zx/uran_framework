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

class ScopedStateObserverAccess::Observation final : public uran_plugin_api::StateObservation
{
public:
  explicit Observation(std::shared_ptr<uran_plugin_api::StateObservation> upstream)
  : upstream_(std::move(upstream)) {}
  void cancel() noexcept override
  {
    std::shared_ptr<uran_plugin_api::StateObservation> upstream;
    {std::lock_guard<std::mutex> lock(mutex_); upstream.swap(upstream_);}
    if (upstream) {upstream->cancel();}
  }
private:
  std::mutex mutex_;
  std::shared_ptr<uran_plugin_api::StateObservation> upstream_;
};

ScopedStateObserverAccess::ScopedStateObserverAccess(
  std::shared_ptr<uran_plugin_api::StateObserverAccess> upstream,
  std::shared_ptr<ScopedPlatformServices> scope)
: upstream_(std::move(upstream)), scope_(std::move(scope)) {}

void ScopedStateObserverAccess::enable()
{
  std::lock_guard<std::mutex> lock(mutex_);
  revoked_ = false;
}

void ScopedStateObserverAccess::revoke() noexcept
{
  std::vector<std::shared_ptr<Observation>> observations;
  {std::lock_guard<std::mutex> lock(mutex_); revoked_ = true; observations.swap(observations_);}
  for (const auto & observation : observations) {if (observation) {observation->cancel();}}
}

std::shared_ptr<uran_plugin_api::StateObservation> ScopedStateObserverAccess::observe(
  const std::string & prefix, uran_plugin_api::StateChangeCallback callback)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (revoked_ || !scope_->enabled()) {return {};}
  auto observation = std::make_shared<Observation>(upstream_->observe(prefix,
    [scope = scope_, callback = std::move(callback)](const auto & field, const auto & value) {
      if (scope->enabled() && callback) {callback(field, value);}
    }));
  observations_.push_back(observation);
  return observation;
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
