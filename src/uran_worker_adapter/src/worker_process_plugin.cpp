#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "uran_worker_adapter/worker_process_plugin.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "uran_plugin_api/plugin_loader.hpp"

extern char ** environ;

namespace uran_worker_adapter
{
namespace
{
constexpr const char * kProtocol = "uran.worker.jsonl.v1";

std::vector<std::string> yaml_strings(const YAML::Node & node, const std::string & field)
{
  std::vector<std::string> result;
  if (!node[field]) {return result;}
  if (!node[field].IsSequence()) {throw std::runtime_error(field + " must be a sequence");}
  for (const auto & item : node[field]) {result.push_back(item.as<std::string>());}
  return result;
}
}

WorkerProcessPlugin::~WorkerProcessPlugin() {stop_process();}

uran_plugin_api::PluginDescriptor WorkerProcessPlugin::descriptor() const
{
  return {"worker", "1.0.0", uran_plugin_api::kPluginApiVersion,
    "uran_worker_adapter/WorkerProcessPlugin"};
}

uran_plugin_api::PluginDescriptor WorkerProtocolPlugin::descriptor() const
{
  return {"protocol", "1.0.0", uran_plugin_api::kPluginApiVersion,
    "uran_worker_adapter/WorkerProtocolPlugin"};
}

nlohmann::json WorkerProcessPlugin::yaml_to_json(const YAML::Node & node)
{
  if (!node || node.IsNull()) {return nullptr;}
  if (node.IsSequence()) {
    auto value = nlohmann::json::array();
    for (const auto & item : node) {value.push_back(yaml_to_json(item));}
    return value;
  }
  if (node.IsMap()) {
    auto value = nlohmann::json::object();
    for (const auto & item : node) {value[item.first.as<std::string>()] = yaml_to_json(item.second);}
    return value;
  }
  const auto scalar = node.Scalar();
  try {return nlohmann::json::parse(scalar);} catch (...) {return scalar;}
}

uran_plugin_api::Status WorkerProcessPlugin::configure(
  const uran_plugin_api::PluginContext & context, const YAML::Node & config)
{
  try {
    command_ = yaml_strings(config, "command");
    message_types_ = yaml_strings(config, "message_types");
    capabilities_ = yaml_strings(config, "capabilities");
    subscriptions_config_ = yaml_strings(config, "state_subscriptions");
    protocol_enabled_ = force_protocol_;
    if (command_.empty() || (message_types_.empty() && !protocol_enabled_)) {
      return uran_plugin_api::Status::failure(
        "worker command and at least message_types or protocol=true are required");
    }
    if (config["environment"]) {
      if (!config["environment"].IsMap()) {throw std::runtime_error("environment must be a map");}
      for (const auto & item : config["environment"]) {
        environment_[item.first.as<std::string>()] = item.second.as<std::string>();
      }
    }
    working_directory_ = config["working_directory"] ? config["working_directory"].as<std::string>() : "";
    request_timeout_ms_ = config["request_timeout_ms"] ? config["request_timeout_ms"].as<int>() : 2000;
    shutdown_timeout_ms_ = config["shutdown_timeout_ms"] ? config["shutdown_timeout_ms"].as<int>() : 2000;
    max_frame_bytes_ = config["max_frame_bytes"] ? config["max_frame_bytes"].as<std::size_t>() : 1024U * 1024U;
    max_pending_frames_ = config["max_pending_frames"] ? config["max_pending_frames"].as<std::size_t>() : 128U;
    if (request_timeout_ms_ <= 0 || shutdown_timeout_ms_ <= 0 ||
      max_frame_bytes_ < 1024U || max_frame_bytes_ > 16U * 1024U * 1024U ||
      max_pending_frames_ == 0 || max_pending_frames_ > 4096U)
    {throw std::runtime_error("invalid worker limits");}
    platform_ = context.platform();
    state_ = context.state_access();
    messages_ = context.messages();
    local_messages_ = context.local_messages();
    observer_ = context.state_observer();
    runtime_snapshot_ = context.runtime_snapshot();
    identity_ = context.identity();
    state_field_ = "plugin." + identity_.instance_id + ".worker_healthy";
    auto result = state_->register_field({state_field_, "bool", "", "", false});
    if (result.ok) {result = state_->set(state_field_, "false", identity_.instance_id);}
    if (!result.ok) {return result;}
    const auto process_config = config["worker_config"] ? yaml_to_json(config["worker_config"]) : nlohmann::json::object();
    result = spawn();
    if (!result.ok) {return result;}
    nlohmann::json reply;
    result = send_request({{"op", "hello"}, {"protocol", kProtocol},
      {"identity", {{"instance_id", identity_.instance_id}, {"device_id", identity_.device_id},
      {"component_id", identity_.component_id}}},
      {"host", {{"sdk_version", uran_plugin_api::kPluginApiVersion},
      {"max_frame_bytes", max_frame_bytes_}}}}, reply);
    if (!result.ok) {stop_process(); return result;}
    if (reply.value("protocol", "") != kProtocol) {
      stop_process(); return uran_plugin_api::Status::failure("worker protocol mismatch");
    }
    plugin_id_ = reply.value("plugin", nlohmann::json::object()).value("id", "");
    plugin_version_ = reply.value("plugin", nlohmann::json::object()).value("version", "0.0.0");
    result = send_request({{"op", "configure"}, {"config", process_config}}, reply);
    if (!result.ok) {stop_process(); return result;}
    configured_ = true;
    post_health(true, "worker configured");
    return uran_plugin_api::Status::success();
  } catch (const std::exception & exception) {
    stop_process();
    return uran_plugin_api::Status::failure(exception.what());
  }
}

uran_plugin_api::Status WorkerProcessPlugin::spawn()
{
  int to_child[2] {-1, -1};
  int from_child[2] {-1, -1};
  if (pipe2(to_child, O_CLOEXEC) != 0 || pipe2(from_child, O_CLOEXEC) != 0) {
    if (to_child[0] >= 0) {close(to_child[0]);}
    if (to_child[1] >= 0) {close(to_child[1]);}
    if (from_child[0] >= 0) {close(from_child[0]);}
    if (from_child[1] >= 0) {close(from_child[1]);}
    return uran_plugin_api::Status::failure(std::strerror(errno));
  }
  std::map<std::string, std::string> child_environment;
  for (char ** item = environ; item && *item; ++item) {
    const std::string entry(*item);
    const auto separator = entry.find('=');
    if (separator != std::string::npos) {
      child_environment[entry.substr(0, separator)] = entry.substr(separator + 1U);
    }
  }
  for (const auto & item : environment_) {child_environment[item.first] = item.second;}
  std::vector<std::string> environment_storage;
  for (const auto & item : child_environment) {
    environment_storage.push_back(item.first + "=" + item.second);
  }
  std::vector<char *> environment;
  for (auto & item : environment_storage) {environment.push_back(item.data());}
  environment.push_back(nullptr);
  std::vector<char *> arguments;
  for (auto & item : command_) {arguments.push_back(item.data());}
  arguments.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  posix_spawnattr_t attributes;
  posix_spawn_file_actions_init(&actions);
  posix_spawnattr_init(&attributes);
  posix_spawn_file_actions_adddup2(&actions, to_child[0], STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&actions, from_child[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&actions, to_child[0]);
  posix_spawn_file_actions_addclose(&actions, to_child[1]);
  posix_spawn_file_actions_addclose(&actions, from_child[0]);
  posix_spawn_file_actions_addclose(&actions, from_child[1]);
  if (!working_directory_.empty()) {
    posix_spawn_file_actions_addchdir_np(&actions, working_directory_.c_str());
  }
  posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
  posix_spawnattr_setpgroup(&attributes, 0);
  pid_t pid = -1;
  const auto spawned = posix_spawnp(&pid, arguments[0], &actions, &attributes,
    arguments.data(), environment.data());
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attributes);
  if (spawned != 0) {
    close(to_child[0]); close(to_child[1]); close(from_child[0]); close(from_child[1]);
    return uran_plugin_api::Status::failure(std::strerror(spawned));
  }
  close(to_child[0]); close(from_child[1]);
  child_pid_ = pid;
  input_fd_ = from_child[0];
  output_fd_ = to_child[1];
  const auto flags = fcntl(output_fd_, F_GETFL, 0);
  if (flags < 0 || fcntl(output_fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
    stop_process(); return uran_plugin_api::Status::failure("cannot configure worker pipe");
  }
  io_stopping_ = false;
  child_reaped_ = false;
  reader_ = std::thread([this] {reader_loop();});
  writer_ = std::thread([this] {writer_loop();});
  return uran_plugin_api::Status::success();
}

std::string WorkerProcessPlugin::error_text(const nlohmann::json & frame, const std::string & fallback)
{
  if (frame.contains("error") && frame["error"].is_object()) {
    return frame["error"].value("code", "worker_error") + ": " +
      frame["error"].value("message", fallback);
  }
  return fallback;
}

uran_plugin_api::Status WorkerProcessPlugin::send_frame(const nlohmann::json & frame)
{
  const auto line = frame.dump();
  if (line.size() + 1U > max_frame_bytes_) {return uran_plugin_api::Status::failure("worker frame exceeds limit");}
  std::lock_guard<std::mutex> write_lock(write_mutex_);
  if (output_fd_ < 0 || io_stopping_) {return uran_plugin_api::Status::failure("worker input is closed");}
  if (write_queue_.size() >= max_pending_frames_ ||
    queued_bytes_ + line.size() + 1U > max_pending_frames_ * max_frame_bytes_)
  {return uran_plugin_api::Status::failure("worker input backpressure limit reached");}
  write_queue_.push_back(line + '\n');
  queued_bytes_ += line.size() + 1U;
  write_changed_.notify_one();
  return uran_plugin_api::Status::success();
}

void WorkerProcessPlugin::writer_loop()
{
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGPIPE);
  pthread_sigmask(SIG_BLOCK, &signals, nullptr);
  while (true) {
    std::string frame;
    {
      std::unique_lock<std::mutex> lock(write_mutex_);
      write_changed_.wait(lock, [this] {return io_stopping_ || !write_queue_.empty();});
      if (io_stopping_) {return;}
      frame = std::move(write_queue_.front());
      write_queue_.pop_front();
      queued_bytes_ -= frame.size();
    }
    std::size_t offset = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(request_timeout_ms_);
    while (offset < frame.size()) {
      struct pollfd descriptor {output_fd_, POLLOUT, 0};
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
      if (io_stopping_) {return;}
      if (remaining <= 0 || poll(&descriptor, 1, static_cast<int>(remaining)) <= 0) {
        post_health(false, "worker input timed out");
        if (child_pid_ > 0) {kill(-child_pid_.load(), SIGTERM);}
        return;
      }
      const auto count = write(output_fd_, frame.data() + offset, frame.size() - offset);
      if (count > 0) {offset += static_cast<std::size_t>(count); continue;}
      if (count < 0 && (errno == EAGAIN || errno == EINTR)) {continue;}
      post_health(false, "worker input is closed");
      if (child_pid_ > 0) {kill(-child_pid_.load(), SIGTERM);}
      return;
    }
  }
}

uran_plugin_api::Status WorkerProcessPlugin::send_request(
  const nlohmann::json & body, nlohmann::json & reply)
{
  const auto request_id = identity_.instance_id + "-" + std::to_string(++request_counter_);
  auto frame = body;
  frame["kind"] = "request";
  frame["request_id"] = request_id;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    replies_[request_id] = {};
  }
  const auto sent = send_frame(frame);
  if (!sent.ok) {std::lock_guard<std::mutex> lock(mutex_); replies_.erase(request_id); return sent;}
  std::unique_lock<std::mutex> lock(mutex_);
  const auto ready = replies_changed_.wait_for(lock, std::chrono::milliseconds(request_timeout_ms_),
    [this, &request_id] {return replies_.at(request_id).ok || child_pid_ < 0;});
  if (!ready || !replies_.at(request_id).ok) {
    replies_.erase(request_id);
    ++error_count_;
    healthy_ = false;
    health_message_ = ready ? "worker exited" : "worker request timed out";
    return uran_plugin_api::Status::failure(health_message_);
  }
  reply = std::move(replies_.at(request_id).frame);
  replies_.erase(request_id);
  if (!reply.value("ok", false)) {return uran_plugin_api::Status::failure(error_text(reply, "worker rejected request"));}
  return uran_plugin_api::Status::success();
}

uran_plugin_api::Status WorkerProcessPlugin::send_async_request(
  const nlohmann::json & body, const std::string & operation_id)
{
  const auto request_id = identity_.instance_id + "-" + std::to_string(++request_counter_);
  auto frame = body;
  frame["kind"] = "request";
  frame["request_id"] = request_id;
  const auto operation_target = body.value("operation_target", body.value("target", nlohmann::json::object()));
  frame.erase("operation_target");
  const AsyncRequest asynchronous{operation_id, body.value("op", "invoke"),
    operation_target,
    body.value("source", nlohmann::json::object())};
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (child_pid_ < 0) {return uran_plugin_api::Status::failure("worker exited");}
    asynchronous_operations_[request_id] = asynchronous;
    if (body.value("op", "") != "publish" && body.value("op", "") != "cancel") {
      operation_targets_[operation_id] = operation_target;
      operation_result_targets_[operation_id] = body.value("source", nlohmann::json::object());
    }
  }
  const auto sent = send_frame(frame);
  if (!sent.ok) {
    std::lock_guard<std::mutex> lock(mutex_);
    asynchronous_operations_.erase(request_id);
    return sent;
  }
  auto timeout = platform_->schedule(std::chrono::milliseconds(request_timeout_ms_), false,
    [this, request_id, asynchronous] {
      bool pending = false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        pending = asynchronous_operations_.erase(request_id) != 0;
        operation_timeouts_.erase(request_id);
      }
      if (pending) {
        post_health(false, "worker request timed out");
        if (asynchronous.operation != "publish") {emit_operation_status(asynchronous, "failed", nlohmann::json::object(),
          {{"code", "worker_timeout"}, {"message", "worker request timed out"},
          {"retryable", true}, {"details", nlohmann::json::object()}});}
      }
    });
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (asynchronous_operations_.count(request_id)) {
      operation_timeouts_[request_id] = std::move(timeout);
    } else if (timeout) {timeout->cancel();}
  }
  return uran_plugin_api::Status::success("worker request queued");
}

void WorkerProcessPlugin::reader_loop()
{
  std::string buffer;
  char chunk[4096];
  bool invalid = false;
  while (!invalid) {
    const auto count = read(input_fd_, chunk, sizeof(chunk));
    if (count < 0 && errno == EINTR) {continue;}
    if (count <= 0) {break;}
    buffer.append(chunk, static_cast<std::size_t>(count));
    std::size_t newline;
    while ((newline = buffer.find('\n')) != std::string::npos) {
      const auto line = buffer.substr(0, newline);
      buffer.erase(0, newline + 1U);
      if (line.size() + 1U > max_frame_bytes_) {post_health(false, "worker frame exceeds limit"); invalid = true; break;}
      auto frame = nlohmann::json::parse(line, nullptr, false);
      if (frame.is_discarded() || !frame.is_object()) {post_health(false, "invalid worker JSON"); invalid = true; break;}
      try {process_frame(frame);}
      catch (const std::exception & exception) {
        post_health(false, std::string("invalid worker frame: ") + exception.what());
        invalid = true;
        break;
      } catch (...) {
        post_health(false, "invalid worker frame");
        invalid = true;
        break;
      }
    }
    if (buffer.size() >= max_frame_bytes_) {post_health(false, "worker frame exceeds limit"); invalid = true;}
  }
  const pid_t pid = child_pid_;
  int status = 0;
  // A worker may close stdout without exiting, or ignore SIGTERM. Always reap
  // the entire process group with a finite deadline, including on malformed IO.
  if (pid > 0) {
    kill(-pid, SIGTERM);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(shutdown_timeout_ms_);
    while (waitpid(pid, &status, WNOHANG) == 0) {
      if (std::chrono::steady_clock::now() >= deadline) {kill(-pid, SIGKILL); break;}
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    kill(-pid, SIGKILL);
  }
  if (pid > 0) {while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}}
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!io_stopping_) {++error_count_; healthy_ = false; health_message_ = "worker exited";}
    child_status_ = status;
    child_reaped_ = true;
    child_pid_ = -1;
  }
  replies_changed_.notify_all();
  process_changed_.notify_all();
}

void WorkerProcessPlugin::process_frame(const nlohmann::json & frame)
{
  const auto kind = frame.value("kind", "");
  if (kind == "reply") {
    const auto request_id = frame.value("request_id", "");
    AsyncRequest async_request;
    std::shared_ptr<uran_plugin_api::Cancellation> timeout;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto it = replies_.find(request_id);
      if (it != replies_.end()) {
        it->second.frame = frame; it->second.ok = true; replies_changed_.notify_all();
        return;
      }
      const auto async = asynchronous_operations_.find(request_id);
      if (async != asynchronous_operations_.end()) {
        async_request = async->second;
        asynchronous_operations_.erase(async);
      }
      const auto pending = operation_timeouts_.find(request_id);
      if (pending != operation_timeouts_.end()) {
        timeout = pending->second;
        operation_timeouts_.erase(pending);
      }
    }
    if (timeout) {timeout->cancel();}
    if (!async_request.operation_id.empty() && !frame.value("ok", false)) {
      post_health(false, error_text(frame, "worker rejected request"));
      if (async_request.operation != "publish") {
        try {platform_->post([this, async_request, error = frame.value("error", nlohmann::json::object())] {
          emit_operation_status(async_request, "rejected", {}, error);
        });} catch (...) {}
      }
    } else if (!async_request.operation_id.empty() && async_request.operation != "publish") {
      const auto status = frame.value("status", "accepted");
      const auto payload = frame.value("result", frame.value("payload", nlohmann::json::object()));
      try {platform_->post([this, async_request, status, payload] {
        emit_operation_status(async_request, status, payload);
      });} catch (...) {}
    }
  } else if (kind == "event") {
    try {if (platform_) {platform_->post([this, frame] {process_event(frame);});}} catch (...) {}
  } else if (kind == "host_request") {
    const auto op = frame.value("op", "");
    if (op == "state.register" || op == "state.set" || op == "state.snapshot" ||
      op == "runtime.snapshot") {
      // StateStore is thread-safe. Lifecycle callbacks may need state access
      // while the serial Runtime executor is waiting for their reply.
      process_host_request(frame);
    } else if (!accepting_host_requests_) {
      static_cast<void>(send_frame({{"kind", "host_reply"},
        {"request_id", frame.value("request_id", "")}, {"ok", false},
        {"error", {{"code", "lifecycle_busy"},
        {"message", "host requests are available only after activation"},
        {"retryable", true}, {"details", nlohmann::json::object()}}}}));
    } else {
      try {if (platform_) {platform_->post([this, frame] {process_host_request(frame);});}} catch (...) {}
    }
  } else {post_health(false, "unexpected worker frame");}
}

void WorkerProcessPlugin::process_event(const nlohmann::json & frame)
{
  if (frame.value("event", "") == "downlink") {
    uran_plugin_api::DownlinkCallback callback;
    {std::lock_guard<std::mutex> lock(mutex_); callback = downlink_callback_;}
    if (!callback || !connected_) {return;}
    const auto message = frame.value("message", nlohmann::json::object());
    auto payload = message.value("payload", nlohmann::json::object());
    const auto target = message.value("target", nlohmann::json::object());
    callback({message.value("message_type", ""), payload.dump(),
      message.value("correlation_id", ""), {},
      {target.value("device_id", ""), target.value("component_id", ""),
      target.value("instance_id", ""), target.value("capability_id", "")}, 0});
    return;
  }
  if (frame.value("event", "") != "operation" || !messages_) {return;}
  const auto operation_id = frame.value("operation_id", "");
  const auto sequence = frame.value("sequence", 0ULL);
  const auto status = frame.value("status", "");
  if (operation_id.empty() || sequence == 0 ||
    (status != "accepted" && status != "running" && status != "succeeded" &&
    status != "failed" && status != "cancelled" && status != "rejected"))
  {post_health(false, "invalid worker operation event"); return;}
  {
    bool invalid_sequence = false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (terminal_operations_.count(operation_id) || sequence <= worker_sequences_[operation_id]) {
      invalid_sequence = true;
    } else {
      worker_sequences_[operation_id] = sequence;
    }
    if (invalid_sequence) {
      ++error_count_;
      healthy_ = false;
      health_message_ = "out-of-order worker operation event";
      return;
    }
  }
  nlohmann::json command_target;
  nlohmann::json result_target;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = operation_targets_.find(operation_id);
    if (found == operation_targets_.end()) {return;}
    command_target = found->second;
    result_target = operation_result_targets_.at(operation_id);
  }
  emit_operation_status({operation_id, "invoke", command_target, result_target}, status,
    frame.value("payload", nlohmann::json::object()),
    frame.value("error", nlohmann::json::object()));
}

void WorkerProcessPlugin::emit_operation_status(const AsyncRequest & request,
  const std::string & status, const nlohmann::json & payload, const nlohmann::json & error)
{
  static const std::set<std::string> allowed{
    "accepted", "running", "succeeded", "failed", "cancelled", "rejected"};
  if (!allowed.count(status)) {post_health(false, "invalid worker operation status"); return;}
  std::uint64_t sequence;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (terminal_operations_.count(request.operation_id)) {return;}
    sequence = ++operation_sequences_[request.operation_id];
    if (status == "succeeded" || status == "failed" || status == "cancelled" || status == "rejected") {
      terminal_operations_.insert(request.operation_id);
      operation_targets_.erase(request.operation_id);
      operation_result_targets_.erase(request.operation_id);
      worker_sequences_.erase(request.operation_id);
      operation_sequences_.erase(request.operation_id);
      terminal_order_.push_back(request.operation_id);
      if (terminal_order_.size() > 4096) {
        terminal_operations_.erase(terminal_order_.front());
        terminal_order_.pop_front();
      }
    }
  }
  const auto timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
    platform_->system_now().time_since_epoch()).count();
  nlohmann::json target = request.target;
  if (!target.is_object()) {target = nlohmann::json::object();}
  const auto normalized_payload = payload.is_object() ? payload : nlohmann::json::object();
  nlohmann::json body{{"schema_version", "1.0"}, {"operation_id", request.operation_id},
    {"sequence", sequence}, {"status", status}, {"timestamp_ns", timestamp},
    {"target", target}, {"payload", normalized_payload}};
  if (!error.empty()) {body["error"] = error;}
  uran_plugin_api::DownlinkEnvelope local;
  local.message_type = "uran.operations.state.v1";
  local.payload = body.dump();
  local.correlation_id = request.operation_id;
  if (request.result_target.is_object()) {
    local.target = {request.result_target.value("device_id", ""),
      request.result_target.value("component_id", ""),
      request.result_target.value("instance_id", ""), ""};
  }
  if (local_messages_) {static_cast<void>(local_messages_->dispatch(local));}
  if (messages_) {
    uran_plugin_api::UplinkEnvelope uplink;
    uplink.message_type = local.message_type;
    uplink.payload = local.payload;
    uplink.correlation_id = local.correlation_id;
    static_cast<void>(messages_->publish(uplink));
  }
}

void WorkerProcessPlugin::process_host_request(const nlohmann::json & frame)
{
  nlohmann::json reply{{"kind", "host_reply"}, {"request_id", frame.value("request_id", "")}};
  uran_plugin_api::Status result = uran_plugin_api::Status::failure("unsupported host request");
  try {
    const auto op = frame.value("op", "");
    if (op == "dispatch" && local_messages_) {
      const auto item = frame.at("message");
      uran_plugin_api::DownlinkEnvelope message;
      message.message_type = item.at("message_type").get<std::string>();
      message.payload = item.value("payload", nlohmann::json::object()).dump();
      message.correlation_id = item.value("correlation_id", "");
      const auto target = item.value("target", nlohmann::json::object());
      message.target = {target.value("device_id", ""), target.value("component_id", ""),
        target.value("instance_id", ""), target.value("capability_id", "")};
      result = local_messages_->dispatch(message);
    } else if (op == "publish" && messages_) {
      const auto item = frame.at("message");
      uran_plugin_api::UplinkEnvelope message;
      message.message_type = item.at("message_type").get<std::string>();
      message.payload = item.value("payload", nlohmann::json::object()).dump();
      message.correlation_id = item.value("correlation_id", "");
      message.urgent = item.value("urgent", false);
      result = messages_->publish(message);
    } else if (op == "state.snapshot") {
      reply["result"] = nlohmann::json::parse(state_->snapshot_json());
      result = uran_plugin_api::Status::success();
    } else if (op == "runtime.snapshot" && runtime_snapshot_) {
      reply["result"] = nlohmann::json::parse(runtime_snapshot_->snapshot_json());
      result = uran_plugin_api::Status::success();
    } else if (op == "state.set") {
      result = state_->set(frame.at("field").get<std::string>(), frame.at("value").dump(),
        identity_.instance_id, frame.value("persistent", false));
    } else if (op == "state.register") {
      result = state_->register_field({frame.at("field").get<std::string>(),
        frame.value("type", "any"), frame.value("unit", ""), "",
        frame.value("persistent", false)});
    }
  } catch (const std::exception & exception) {result = uran_plugin_api::Status::failure(exception.what());}
  reply["ok"] = result.ok;
  if (result.ok && !reply.contains("result")) {reply["result"] = {{"message", result.message}};}
  if (!result.ok) {reply["error"] = {{"code", "host_error"}, {"message", result.message},
    {"retryable", false}, {"details", nlohmann::json::object()}};}
  static_cast<void>(send_frame(reply));
}

void WorkerProcessPlugin::post_health(bool healthy, std::string message)
{
  std::lock_guard<std::mutex> lock(mutex_);
  healthy_ = healthy;
  health_message_ = std::move(message);
  if (!healthy) {++error_count_;}
}

uran_plugin_api::Status WorkerProcessPlugin::activate()
{
  nlohmann::json reply;
  auto result = send_request({{"op", "activate"}}, reply);
  if (!result.ok) {return result;}
  active_ = true;
  result = state_->set(state_field_, "true", identity_.instance_id);
  if (!result.ok) {return result;}
  if (observer_) {
    for (const auto & prefix : subscriptions_config_) {
      const auto subscription_id = "state-" + std::to_string(subscriptions_.size() + 1U);
      auto send_state = [this, subscription_id](const auto & field, const auto & value) {
          const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
            value.updated_at.time_since_epoch()).count();
          const auto parsed = nlohmann::json::parse(value.value_json, nullptr, false);
          std::uint64_t sequence;
          {std::lock_guard<std::mutex> lock(subscription_mutex_);
            sequence = ++subscription_sequences_[subscription_id];}
          static_cast<void>(send_frame({{"kind", "event"}, {"event", "state"},
            {"subscription_id", subscription_id},
            {"sequence", sequence}, {"field", field},
            {"value", {{"value", parsed.is_discarded() ? nlohmann::json(value.value_json) : parsed},
            {"source", value.source}, {"updated_at_ms", milliseconds},
            {"updated_at_ns", std::chrono::duration_cast<std::chrono::nanoseconds>(
              value.updated_at.time_since_epoch()).count()},
            {"persistent", value.persistent}}}}));
        };
      subscriptions_.push_back(observer_->observe(prefix, send_state));
      result = send_request({{"op", "subscribe"}, {"subscription_id", subscription_id},
        {"field_prefix", prefix}}, reply);
      if (!result.ok) {return result;}
      for (const auto & item : state_->snapshot()) {
        if (prefix.empty() || item.first.rfind(prefix, 0) == 0) {send_state(item.first, item.second);}
      }
    }
  }
  if (protocol_enabled_) {
    result = connect();
    if (!result.ok) {active_ = false; return result;}
  }
  accepting_host_requests_ = true;
  post_health(true, "worker active");
  return uran_plugin_api::Status::success();
}

uran_plugin_api::Status WorkerProcessPlugin::deactivate()
{
  accepting_host_requests_ = false;
  for (auto & subscription : subscriptions_) {if (subscription) {subscription->cancel();}}
  subscriptions_.clear();
  if (connected_) {static_cast<void>(disconnect());}
  nlohmann::json reply;
  auto result = child_pid_ >= 0 ? send_request({{"op", "deactivate"}}, reply) :
    uran_plugin_api::Status::success("worker already exited");
  if (!result.ok) {
    // A failed worker must not prevent Runtime shutdown/unload. The failure
    // remains in health; stopping and reaping its process completes deactivation.
    post_health(false, result.message);
    stop_process();
    result = uran_plugin_api::Status::success("failed worker forcibly stopped");
  }
  active_ = false;
  if (state_) {static_cast<void>(state_->set(state_field_, "false", identity_.instance_id));}
  return result;
}

uran_plugin_api::Status WorkerProcessPlugin::cleanup()
{
  if (active_) {return uran_plugin_api::Status::failure("worker must be inactive before cleanup");}
  if (child_pid_ >= 0) {
    nlohmann::json reply;
    static_cast<void>(send_request({{"op", "shutdown"}}, reply));
  }
  stop_process();
  configured_ = false;
  return uran_plugin_api::Status::success();
}

void WorkerProcessPlugin::stop_process() noexcept
{
  pid_t pid;
  std::map<std::string, std::shared_ptr<uran_plugin_api::Cancellation>> timeouts;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    timeouts.swap(operation_timeouts_);
    asynchronous_operations_.clear();
  }
  for (auto & item : timeouts) {if (item.second) {item.second->cancel();}}
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pid = child_pid_;
  }
  {
    std::lock_guard<std::mutex> write_lock(write_mutex_);
    io_stopping_ = true;
    write_queue_.clear();
    queued_bytes_ = 0;
    write_changed_.notify_all();
  }
  if (writer_.joinable() && writer_.get_id() != std::this_thread::get_id()) {writer_.join();}
  if (output_fd_ >= 0) {close(output_fd_); output_fd_ = -1;}
  if (pid > 0) {
    kill(-pid, SIGTERM);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(shutdown_timeout_ms_);
    while (std::chrono::steady_clock::now() < deadline && !child_reaped_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!child_reaped_) {
      kill(-pid, SIGKILL);
    }
  }
  if (reader_.joinable() && reader_.get_id() != std::this_thread::get_id()) {reader_.join();}
  if (input_fd_ >= 0) {close(input_fd_); input_fd_ = -1;}
  std::lock_guard<std::mutex> lock(mutex_);
  child_pid_ = -1;
  replies_changed_.notify_all();
}

uran_plugin_api::PluginHealth WorkerProcessPlugin::health() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return {healthy_ ? uran_plugin_api::PluginHealth::Level::healthy :
    (configured_ ? uran_plugin_api::PluginHealth::Level::error : uran_plugin_api::PluginHealth::Level::stopped),
    health_message_, platform_ ? platform_->system_now() : std::chrono::system_clock::time_point{}, error_count_};
}

uran_plugin_api::PluginCapabilities WorkerProcessPlugin::capabilities() const
{
  auto features = capabilities_;
  if (protocol_enabled_) {features.push_back("protocol.worker");}
  return {features, {"scheduler.serial", "process.worker.jsonl.v1"}, {state_field_}};
}

std::vector<std::string> WorkerProcessPlugin::message_types() const
{
  auto types = message_types_;
  if (std::find(types.begin(), types.end(), "uran.operations.state.v1") == types.end()) {
    types.push_back("uran.operations.state.v1");
  }
  return types;
}

uran_plugin_api::Status WorkerProcessPlugin::handle(const uran_plugin_api::DownlinkEnvelope & message)
{
  if (!active_) {return uran_plugin_api::Status::failure("worker is inactive");}
  auto payload = nlohmann::json::parse(message.payload, nullptr, false);
  if (payload.is_discarded()) {return uran_plugin_api::Status::failure("worker payload is not JSON");}
  if (message.message_type == "uran.operations.state.v1") {
    // Provider results are delivered to the requesting worker as an SDK event.
    // They are deliberately not invoked as a second worker operation.
    return send_frame({{"kind", "event"}, {"event", "operation_result"},
      {"payload", payload}, {"source", {{"instance_id", message.source.instance_id},
      {"device_id", message.source.device_id}, {"component_id", message.source.component_id}}}});
  }
  const bool cancelling = message.message_type == "uran.operations.cancel.v1";
  const auto operation_id = cancelling && payload.is_object() && payload.contains("operation_id") ?
    payload.at("operation_id").get<std::string>() :
    (!message.correlation_id.empty() ? message.correlation_id :
    identity_.instance_id + "-op-" + std::to_string(++request_counter_));
  if (operation_id.empty()) {return uran_plugin_api::Status::failure("operation_id is required");}
  if (!cancelling) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (operation_targets_.count(operation_id) || terminal_operations_.count(operation_id)) {
      return uran_plugin_api::Status::failure("duplicate operation_id");
    }
  }
  nlohmann::json target = payload.is_object() && payload.contains("target") ?
    payload.at("target") : nlohmann::json{{"device_id", identity_.device_id},
      {"component_id", identity_.component_id}};
  if (cancelling) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (operation_targets_.count(operation_id)) {target = operation_targets_.at(operation_id);}
  }
  auto result = send_async_request({{"op", cancelling ? "cancel" : "invoke"},
    {"operation_id", operation_id}, {"message_type", message.message_type},
    {"operation_target", target},
    {"target", {{"device_id", message.target.device_id}, {"component_id", message.target.component_id},
      {"instance_id", message.target.instance_id}, {"capability_id", message.target.capability_id}}},
    {"source", {{"instance_id", message.source.instance_id},
    {"device_id", message.source.device_id}, {"component_id", message.source.component_id}}},
    {"payload", payload}, {"correlation_id", message.correlation_id}}, operation_id);
  if (!result.ok) {post_health(false, result.message);}
  return result;
}

uran_plugin_api::Status WorkerProcessPlugin::connect()
{
  if (!protocol_enabled_) {return uran_plugin_api::Status::failure("worker protocol mode is disabled");}
  nlohmann::json reply;
  const auto result = send_request({{"op", "connect"}}, reply);
  if (result.ok) {connected_ = true;}
  return result;
}

uran_plugin_api::Status WorkerProcessPlugin::disconnect()
{
  if (!connected_) {return uran_plugin_api::Status::success();}
  nlohmann::json reply;
  const auto result = child_pid_ >= 0 ? send_request({{"op", "disconnect"}}, reply) :
    uran_plugin_api::Status::success("worker already exited");
  connected_ = false;
  return result;
}

uran_plugin_api::Status WorkerProcessPlugin::publish(const uran_plugin_api::UplinkEnvelope & message)
{
  if (!connected_) {return uran_plugin_api::Status::failure("worker protocol is disconnected");}
  auto payload = nlohmann::json::parse(message.payload, nullptr, false);
  if (payload.is_discarded()) {return uran_plugin_api::Status::failure("protocol payload is not JSON");}
  const auto operation_id = !message.correlation_id.empty() ? message.correlation_id :
    identity_.instance_id + "-publish-" + std::to_string(++request_counter_);
  return send_async_request({{"op", "publish"}, {"operation_id", operation_id},
    {"message", {{"message_type", message.message_type}, {"payload", payload},
    {"correlation_id", message.correlation_id}, {"urgent", message.urgent},
    {"source", {{"instance_id", message.source.instance_id},
    {"device_id", message.source.device_id}, {"component_id", message.source.component_id}}}}},
    {"source", {{"instance_id", message.source.instance_id},
    {"device_id", message.source.device_id}, {"component_id", message.source.component_id}}}},
    operation_id);
}

void WorkerProcessPlugin::set_downlink_callback(uran_plugin_api::DownlinkCallback callback)
{
  std::lock_guard<std::mutex> lock(mutex_);
  downlink_callback_ = std::move(callback);
}

}
