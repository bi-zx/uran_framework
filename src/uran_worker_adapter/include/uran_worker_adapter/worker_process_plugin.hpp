#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include "uran_plugin_api/message_plugin.hpp"

namespace uran_worker_adapter
{

/**
 * A lifecycle-managed JSONL worker. The process is deliberately an ordinary
 * child, so Python/ROS/vendor ABI conflicts stay outside UranRuntime.
 */
class WorkerProcessPlugin : public uran_plugin_api::MessagePlugin,
  public uran_plugin_api::ProtocolPlugin
{
public:
  explicit WorkerProcessPlugin(bool protocol = false) : force_protocol_(protocol) {}
  ~WorkerProcessPlugin() override;

  uran_plugin_api::PluginDescriptor descriptor() const override;
  uran_plugin_api::Status configure(
    const uran_plugin_api::PluginContext &, const YAML::Node &) override;
  uran_plugin_api::Status activate() override;
  uran_plugin_api::Status deactivate() override;
  uran_plugin_api::Status cleanup() override;
  uran_plugin_api::PluginHealth health() const override;
  uran_plugin_api::PluginCapabilities capabilities() const override;
  std::vector<std::string> message_types() const override;
  uran_plugin_api::Status handle(const uran_plugin_api::DownlinkEnvelope &) override;
  uran_plugin_api::Status connect() override;
  uran_plugin_api::Status disconnect() override;
  uran_plugin_api::Status publish(const uran_plugin_api::UplinkEnvelope &) override;
  void set_downlink_callback(uran_plugin_api::DownlinkCallback) override;

private:
  struct Reply {bool ok{false}; nlohmann::json frame;};
  struct AsyncRequest
  {
    std::string operation_id;
    std::string operation;
    nlohmann::json target{nlohmann::json::object()};
    nlohmann::json result_target{nlohmann::json::object()};
  };

  uran_plugin_api::Status spawn();
  void stop_process() noexcept;
  uran_plugin_api::Status send_request(const nlohmann::json &, nlohmann::json &);
  uran_plugin_api::Status send_async_request(
    const nlohmann::json &, const std::string & operation_id);
  uran_plugin_api::Status send_frame(const nlohmann::json &);
  void reader_loop();
  void writer_loop();
  void process_frame(const nlohmann::json &);
  void process_event(const nlohmann::json &);
  void process_host_request(const nlohmann::json &);
  void emit_operation_status(const AsyncRequest &, const std::string &,
    const nlohmann::json & payload = nlohmann::json::object(),
    const nlohmann::json & error = nlohmann::json::object());
  void post_health(bool healthy, std::string message);
  static nlohmann::json yaml_to_json(const YAML::Node &);
  static std::string error_text(const nlohmann::json &, const std::string & fallback);

  mutable std::mutex mutex_;
  std::mutex write_mutex_;
  std::condition_variable write_changed_;
  std::condition_variable process_changed_;
  std::deque<std::string> write_queue_;
  std::size_t queued_bytes_{0};
  std::condition_variable replies_changed_;
  std::map<std::string, Reply> replies_;
  std::map<std::string, AsyncRequest> asynchronous_operations_;
  std::map<std::string, std::uint64_t> operation_sequences_;
  std::map<std::string, std::uint64_t> worker_sequences_;
  std::map<std::string, nlohmann::json> operation_targets_;
  std::map<std::string, nlohmann::json> operation_result_targets_;
  std::map<std::string, std::shared_ptr<uran_plugin_api::Cancellation>> operation_timeouts_;
  std::set<std::string> terminal_operations_;
  std::deque<std::string> terminal_order_;
  std::shared_ptr<uran_plugin_api::PlatformServices> platform_;
  std::shared_ptr<uran_plugin_api::StateAccess> state_;
  std::shared_ptr<uran_plugin_api::MessageAccess> messages_;
  std::shared_ptr<uran_plugin_api::LocalMessageAccess> local_messages_;
  std::shared_ptr<uran_plugin_api::StateObserverAccess> observer_;
  std::vector<std::shared_ptr<uran_plugin_api::StateObservation>> subscriptions_;
  uran_plugin_api::PluginIdentity identity_;
  std::string state_field_;
  std::vector<std::string> command_;
  std::map<std::string, std::string> environment_;
  std::string working_directory_;
  std::vector<std::string> message_types_;
  std::vector<std::string> capabilities_;
  std::vector<std::string> subscriptions_config_;
  std::string plugin_id_;
  std::string plugin_version_{"0.0.0"};
  int request_timeout_ms_{2000};
  int shutdown_timeout_ms_{2000};
  std::size_t max_frame_bytes_{1024U * 1024U};
  std::size_t max_pending_frames_{128};
  std::atomic<std::uint64_t> request_counter_{0};
  std::map<std::string, std::uint64_t> subscription_sequences_;
  std::mutex subscription_mutex_;
  std::atomic<pid_t> child_pid_{-1};
  int input_fd_{-1};
  int output_fd_{-1};
  std::thread reader_;
  std::thread writer_;
  std::atomic<bool> io_stopping_{false};
  std::atomic<bool> child_reaped_{false};
  int child_status_{0};
  bool configured_{false};
  std::atomic<bool> active_{false};
  bool protocol_enabled_{false};
  std::atomic<bool> connected_{false};
  std::atomic<bool> accepting_host_requests_{false};
  uran_plugin_api::DownlinkCallback downlink_callback_;
  bool force_protocol_{false};
  bool healthy_{false};
  std::string health_message_;
  std::uint64_t error_count_{0};
};

class WorkerProtocolPlugin final : public WorkerProcessPlugin
{
public:
  WorkerProtocolPlugin() : WorkerProcessPlugin(true) {}
  uran_plugin_api::PluginDescriptor descriptor() const override;
};

}
