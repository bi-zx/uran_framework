#include "uran_platform_native/native_platform.hpp"

#include <condition_variable>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace uran_platform_native
{
namespace
{
struct Job final : uran_plugin_api::Cancellation
{
  std::mutex mutex;
  std::condition_variable idle;
  bool cancelled{false};
  bool running{false};
  std::thread::id runner;
  std::function<void()> callback;
  std::chrono::steady_clock::time_point due;
  std::chrono::milliseconds period{0};

  void cancel() noexcept override
  {
    std::unique_lock<std::mutex> lock(mutex);
    cancelled = true;
    callback = nullptr;
    if (runner != std::this_thread::get_id()) {
      idle.wait(lock, [this] {return !running;});
    }
  }
};
}

struct NativePlatform::Impl
{
  std::mutex mutex;
  std::condition_variable wake;
  std::map<std::size_t, std::shared_ptr<Job>> jobs;
  std::size_t next_id{0};
  bool stopping{false};
  std::thread worker;
  std::thread::id worker_id;

  void run()
  {
    std::unique_lock<std::mutex> lock(mutex);
    while (!stopping) {
      auto next = jobs.end();
      for (auto it = jobs.begin(); it != jobs.end();) {
        const auto retained = it->second;
        std::lock_guard<std::mutex> job_lock(retained->mutex);
        if (it->second->cancelled) {it = jobs.erase(it); continue;}
        if (next == jobs.end() || it->second->due < next->second->due) {next = it;}
        ++it;
      }
      if (next == jobs.end()) {wake.wait(lock); continue;}
      const auto now = std::chrono::steady_clock::now();
      if (next->second->due > now) {wake.wait_until(lock, next->second->due); continue;}
      const auto job = next->second;
      if (job->period.count() == 0) {jobs.erase(next);}
      else {job->due = now + job->period;}
      lock.unlock();
      std::function<void()> callback;
      {
        std::lock_guard<std::mutex> job_lock(job->mutex);
        if (!job->cancelled) {
          job->running = true;
          job->runner = std::this_thread::get_id();
          callback = job->callback;
        }
      }
      if (callback) {try {callback();} catch (...) { /* isolate callbacks */ }}
      // Release code references before cancellation waiters may unload a library.
      callback = nullptr;
      {
        std::lock_guard<std::mutex> job_lock(job->mutex);
        job->running = false;
        if (job->period.count() == 0) {job->callback = nullptr; job->cancelled = true;}
        job->idle.notify_all();
      }
      lock.lock();
    }
  }
};

NativePlatform::NativePlatform() : impl_(std::make_unique<Impl>())
{
  impl_->worker = std::thread([this] {impl_->run();});
  impl_->worker_id = impl_->worker.get_id();
}
NativePlatform::~NativePlatform() {shutdown();}
void NativePlatform::shutdown()
{
  // The host owns the platform and destroys it after Runtime, outside its executor.
  if (on_executor_thread()) {throw std::logic_error("destroy platform from its host thread");}
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->stopping = true;
    impl_->wake.notify_all();
  }
  if (impl_->worker.joinable()) {impl_->worker.join();}
  std::map<std::size_t, std::shared_ptr<Job>> jobs;
  {std::lock_guard<std::mutex> lock(impl_->mutex); jobs.swap(impl_->jobs);}
  for (const auto & item : jobs) {item.second->cancel();}
}
void NativePlatform::log(
  uran_plugin_api::LogLevel level, const std::string & source, const std::string & text)
{
  static std::mutex output;
  std::lock_guard<std::mutex> lock(output);
  std::clog << '[' << static_cast<int>(level) << "] " << source << ": " << text << '\n';
}
std::chrono::system_clock::time_point NativePlatform::system_now() const
{return std::chrono::system_clock::now();}
std::chrono::steady_clock::time_point NativePlatform::steady_now() const
{return std::chrono::steady_clock::now();}
std::vector<std::string> NativePlatform::interfaces() const
{return {"log", "clock.system", "clock.steady", "scheduler.serial", "timer"};}
bool NativePlatform::on_executor_thread() const noexcept
{return std::this_thread::get_id() == impl_->worker_id;}
std::shared_ptr<uran_plugin_api::Cancellation> NativePlatform::post(std::function<void()> callback)
{return schedule(std::chrono::milliseconds(0), false, std::move(callback));}
std::shared_ptr<uran_plugin_api::Cancellation> NativePlatform::schedule(
  std::chrono::milliseconds delay, bool repeat, std::function<void()> callback)
{
  if (!callback || delay.count() < 0 || (repeat && delay.count() == 0)) {
    throw std::invalid_argument("invalid scheduled callback or period");
  }
  auto job = std::make_shared<Job>();
  job->callback = std::move(callback);
  job->due = std::chrono::steady_clock::now() + delay;
  if (repeat) {job->period = delay;}
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->stopping) {throw std::runtime_error("platform scheduler stopped");}
  impl_->jobs.emplace(++impl_->next_id, job);
  impl_->wake.notify_all();
  return job;
}
}
