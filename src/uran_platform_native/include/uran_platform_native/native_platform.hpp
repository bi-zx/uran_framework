#pragma once

#include <memory>
#include "uran_plugin_api/platform_services.hpp"

namespace uran_platform_native
{
/** Standard C++ serial scheduler. A host may override logging/clock services. */
class NativePlatform : public uran_plugin_api::PlatformServices
{
public:
  NativePlatform();
  ~NativePlatform() override;
  void shutdown();
  void log(uran_plugin_api::LogLevel, const std::string &, const std::string &) override;
  std::chrono::system_clock::time_point system_now() const override;
  std::chrono::steady_clock::time_point steady_now() const override;
  std::vector<std::string> interfaces() const override;
  bool on_executor_thread() const noexcept override;
  std::shared_ptr<uran_plugin_api::Cancellation> post(std::function<void()>) override;
  std::shared_ptr<uran_plugin_api::Cancellation> schedule(
    std::chrono::milliseconds, bool, std::function<void()>) override;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
