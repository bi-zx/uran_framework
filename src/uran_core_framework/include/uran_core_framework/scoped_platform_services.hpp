#pragma once

#include <mutex>
#include <future>
#include <functional>
#include "uran_plugin_api/platform_services.hpp"
#include "uran_plugin_api/runtime_access.hpp"
#include "uran_plugin_api/protocol_plugin.hpp"

namespace uran_core_framework
{

/** Tracks callbacks belonging to one instance; revoke before destroying code. */
class ScopedPlatformServices final : public uran_plugin_api::PlatformServices
{
public:
  ScopedPlatformServices(std::shared_ptr<PlatformServices> upstream, std::string owner, bool enabled = true);
  ~ScopedPlatformServices() override;
  void enable();
  bool enabled() const;
  void revoke() noexcept;
  void log(uran_plugin_api::LogLevel, const std::string &, const std::string &) override;
  std::chrono::system_clock::time_point system_now() const override;
  std::chrono::steady_clock::time_point steady_now() const override;
  std::vector<std::string> interfaces() const override;
  bool on_executor_thread() const noexcept override;
  std::shared_ptr<uran_plugin_api::Cancellation> post(std::function<void()>) override;
  std::shared_ptr<uran_plugin_api::Cancellation> schedule(
    std::chrono::milliseconds, bool, std::function<void()>) override;
private:
  std::shared_ptr<PlatformServices> upstream_;
  std::string owner_;
  mutable std::mutex mutex_;
  bool enabled_{true};
  std::vector<std::weak_ptr<uran_plugin_api::Cancellation>> tasks_;
};

class ScopedMessageAccess final : public uran_plugin_api::MessageAccess
{
public:
  ScopedMessageAccess(std::shared_ptr<uran_plugin_api::MessageAccess> upstream,
    uran_plugin_api::PluginIdentity identity, std::shared_ptr<ScopedPlatformServices> scope)
  : upstream_(std::move(upstream)), identity_(std::move(identity)), scope_(std::move(scope)) {}
  uran_plugin_api::Status publish(const uran_plugin_api::UplinkEnvelope & message,
    const std::string & preferred = {}) override
  {
    if (!scope_->enabled()) {return uran_plugin_api::Status::failure("instance is not active");}
    auto identified = message;
    identified.source = identity_;
    auto invoke = [upstream = upstream_, scope = scope_, identified, preferred] {
      return scope->enabled() ? upstream->publish(identified, preferred) :
        uran_plugin_api::Status::failure("instance is not active");
    };
    if (scope_->on_executor_thread()) {return invoke();}
    try {
      auto task = std::make_shared<std::packaged_task<uran_plugin_api::Status()>>(invoke);
      auto result = task->get_future();
      const auto pending = scope_->post([task] {(*task)();});
      task.reset();
      return result.get();
    } catch (const std::exception & exception) {
      return uran_plugin_api::Status::failure(exception.what());
    }
  }
private:
  std::shared_ptr<uran_plugin_api::MessageAccess> upstream_;
  uran_plugin_api::PluginIdentity identity_;
  std::shared_ptr<ScopedPlatformServices> scope_;
};

class ScopedLocalMessageAccess final : public uran_plugin_api::LocalMessageAccess
{
public:
  ScopedLocalMessageAccess(std::shared_ptr<uran_plugin_api::LocalMessageAccess> upstream,
    uran_plugin_api::PluginIdentity identity, std::shared_ptr<ScopedPlatformServices> scope)
  : upstream_(std::move(upstream)), identity_(std::move(identity)), scope_(std::move(scope)) {}
  uran_plugin_api::Status dispatch(const uran_plugin_api::DownlinkEnvelope & message) override
  {
    if (!scope_->enabled()) {return uran_plugin_api::Status::failure("instance is not active");}
    auto identified = message;
    identified.source = identity_;
    identified.source_generation = 0;
    auto invoke = [upstream = upstream_, scope = scope_, identified] {
      return scope->enabled() ? upstream->dispatch(identified) :
        uran_plugin_api::Status::failure("instance is not active");
    };
    if (scope_->on_executor_thread()) {return invoke();}
    try {
      auto task = std::make_shared<std::packaged_task<uran_plugin_api::Status()>>(invoke);
      auto result = task->get_future();
      const auto pending = scope_->post([task] {(*task)();});
      task.reset();
      return result.get();
    } catch (const std::exception & exception) {
      return uran_plugin_api::Status::failure(exception.what());
    }
  }
private:
  std::shared_ptr<uran_plugin_api::LocalMessageAccess> upstream_;
  uran_plugin_api::PluginIdentity identity_;
  std::shared_ptr<ScopedPlatformServices> scope_;
};

class ScopedStateObserverAccess final : public uran_plugin_api::StateObserverAccess
{
public:
  ScopedStateObserverAccess(std::shared_ptr<uran_plugin_api::StateObserverAccess> upstream,
    std::shared_ptr<ScopedPlatformServices> scope);
  ~ScopedStateObserverAccess() override {revoke();}
  void enable();
  void revoke() noexcept;
  std::shared_ptr<uran_plugin_api::StateObservation> observe(
    const std::string & prefix, uran_plugin_api::StateChangeCallback callback) override;
private:
  class Observation;
  std::shared_ptr<uran_plugin_api::StateObserverAccess> upstream_;
  std::shared_ptr<ScopedPlatformServices> scope_;
  std::mutex mutex_;
  std::vector<std::shared_ptr<Observation>> observations_;
  bool revoked_{false};
};
}
