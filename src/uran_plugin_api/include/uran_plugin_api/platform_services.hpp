#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace uran_plugin_api
{

enum class LogLevel {debug, info, warning, error};

/** Cancellation is idempotent. It releases pending callbacks before returning.
 * Hosts serialize plugin callbacks and lifecycle operations on the executor. */
class Cancellation
{
public:
  virtual ~Cancellation() = default;
  virtual void cancel() noexcept = 0;
};

/** Platform boundary: no Node, Topic, Executor or operating-system handles. */
class PlatformServices
{
public:
  virtual ~PlatformServices() = default;
  virtual void log(LogLevel level, const std::string & source, const std::string & text) = 0;
  virtual std::chrono::system_clock::time_point system_now() const = 0;
  virtual std::chrono::steady_clock::time_point steady_now() const = 0;
  virtual std::vector<std::string> interfaces() const = 0;
  virtual bool on_executor_thread() const noexcept = 0;
  virtual std::shared_ptr<Cancellation> post(std::function<void()> callback) = 0;
  virtual std::shared_ptr<Cancellation> schedule(
    std::chrono::milliseconds delay, bool repeat, std::function<void()> callback) = 0;
};

}
