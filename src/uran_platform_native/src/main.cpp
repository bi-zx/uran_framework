#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#if __has_include(<filesystem>)
#include <filesystem>
namespace uran_fs = std::filesystem;
#else
#include <experimental/filesystem>
namespace uran_fs = std::experimental::filesystem;
#endif
#include <iostream>
#include <fstream>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <nlohmann/json.hpp>
#include "uran_core_framework/runtime.hpp"
#include "uran_platform_native/native_platform.hpp"
#include "uran_platform_native/file_state_storage.hpp"
#include "uran_platform_native/native_plugin_loader.hpp"

namespace
{
using Json = nlohmann::json;
volatile std::sig_atomic_t stopping = 0;
void stop_signal(int) {stopping = 1;}
std::mutex output_mutex;
void output(const Json & message)
{
  std::lock_guard<std::mutex> lock(output_mutex);
  std::cout << message.dump() << std::endl;
}
Json status(const uran_plugin_api::Status & result)
{return {{"ok", result.ok}, {"message", result.message}};}

Json command(const Json & request, uran_core_framework::UranRuntime & runtime,
  const std::shared_ptr<uran_platform_native::NativePluginLoader> & loader)
{
  try {
    const auto operation = request.at("op").get<std::string>();
    if (operation == "status") {return {{"ok", true}, {"result", Json::parse(runtime.diagnostics())}};}
    if (operation == "snapshot") {return {{"ok", true}, {"result", Json::parse(runtime.state_snapshot())}};}
    if (operation == "snapshot_document") {
      return {{"ok", true}, {"result", Json::parse(runtime.state_snapshot_document())}};
    }
    if (operation == "classes") {return {{"ok", true}, {"result", loader->available_classes()}};}
    if (operation == "providers") {
      Json found = Json::array();
      for (const auto & item : runtime.providers(request.at("capability"),
        request.value("device_id", ""), request.value("component_id", ""))) {
        found.push_back({{"instance_id", item.identity.instance_id},
          {"device_id", item.identity.device_id}, {"component_id", item.identity.component_id},
          {"capability_id", item.capability_id}, {"priority", item.priority}});
      }
      return {{"ok", true}, {"result", found}};
    }
    if (operation == "load") {
      if (request.contains("manifest")) {
        const auto added = loader->add_manifest(request.at("manifest"), request.value("replace", false));
        if (!added.ok) {return status(added);}
      }
      return status(runtime.load(YAML::Load(request.at("plugin").dump())));
    }
    if (operation == "activate") {return status(runtime.activate(request.at("instance_id")));}
    if (operation == "deactivate") {return status(runtime.deactivate(request.at("instance_id")));}
    if (operation == "unload") {return status(runtime.unload(request.at("instance_id")));}
    if (operation == "primary") {return status(runtime.set_primary_protocol(request.at("instance_id")));}
    if (operation == "write") {
      return status(runtime.write_state(request.at("field"), request.at("value").dump(),
        "external.native", request.value("persistent", false)));
    }
    if (operation == "publish") {
      return status(runtime.publish({request.at("message_type"), request.at("payload").dump(),
        request.value("correlation_id", ""), request.value("urgent", false)},
        request.value("preferred_protocol_instance", "")));
    }
    if (operation == "dispatch") {
      uran_plugin_api::DownlinkEnvelope message;
      message.message_type = request.at("message_type");
      message.payload = request.at("payload").dump();
      message.correlation_id = request.value("correlation_id", "");
      message.source = {"native_host", "runtime", "host"};
      const auto target = request.value("target", Json::object());
      message.target = {target.value("device_id", ""), target.value("component_id", ""),
        target.value("instance_id", ""), target.value("capability_id", "")};
      return status(runtime.dispatch(message));
    }
    if (operation == "shutdown") {stopping = 1; return status(runtime.stop());}
    return {{"ok", false}, {"message", "unknown operation: " + operation}};
  } catch (const std::exception & exception) {return {{"ok", false}, {"message", exception.what()}};}
}

class ControlSocket
{
public:
  explicit ControlSocket(const std::string & path) : path_(path)
  {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof(address.sun_path)) {throw std::runtime_error("invalid control socket path");}
    if (uran_fs::exists(path)) {throw std::runtime_error("control socket path already exists: " + path);}
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    descriptor_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (descriptor_ < 0) {throw std::runtime_error("cannot create control socket");}
    const auto mask = umask(0077);
    const auto bound = bind(descriptor_, reinterpret_cast<sockaddr *>(&address), sizeof(address));
    umask(mask);
    if (bound != 0 || listen(descriptor_, 8) != 0) {
      close(descriptor_);
      if (bound == 0) {unlink(path.c_str());}
      throw std::runtime_error("cannot bind/listen on control socket: " + path);
    }
  }
  ~ControlSocket() {close(descriptor_); unlink(path_.c_str());}
  void serve(uran_core_framework::UranRuntime & runtime,
    const std::shared_ptr<uran_platform_native::NativePluginLoader> & loader)
  {
    pollfd available{descriptor_, POLLIN, 0};
    if (poll(&available, 1, 100) <= 0) {return;}
    const int client = accept4(descriptor_, nullptr, nullptr, SOCK_CLOEXEC);
    if (client < 0) {return;}
    timeval timeout{2, 0};
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    std::string line;
    char buffer[4096];
    while (line.size() <= 1024 * 1024 && line.find('\n') == std::string::npos) {
      const auto count = recv(client, buffer, sizeof(buffer), 0);
      if (count <= 0) {break;}
      line.append(buffer, static_cast<std::size_t>(count));
    }
    Json response;
    try {
      if (line.size() > 1024 * 1024 || line.find('\n') == std::string::npos) {
        throw std::runtime_error("expected one JSON line, maximum 1 MiB");
      }
      response = command(Json::parse(line.substr(0, line.find('\n'))), runtime, loader);
    } catch (const std::exception & exception) {response = {{"ok", false}, {"message", exception.what()}};}
    const auto bytes = response.dump() + "\n";
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const auto sent = send(client, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
      if (sent <= 0) {break;}
      offset += static_cast<std::size_t>(sent);
    }
    close(client);
  }
private:
  std::string path_;
  int descriptor_{-1};
};
}

int main(int argc, char ** argv)
{
  try {
    std::string config_path;
    std::string control_path;
    std::string state_path;
    std::vector<std::string> manifests;
    std::vector<std::string> directories;
    std::vector<std::string> indexes;
    for (int index = 1; index < argc; ++index) {
      const std::string argument = argv[index];
      if (argument == "--help") {
        std::cout << "uran_runtime [--config FILE] [--manifest FILE] [--plugin-dir DIR] [--plugin-index FILE] [--state-file FILE] [--control-socket PATH]\n"
                     "Without a control socket, reads one JSON management command per stdin line.\n";
        return 0;
      }
      if (index + 1 >= argc) {throw std::runtime_error("missing argument for " + argument);}
      const std::string value = argv[++index];
      if (argument == "--config") {config_path = value;}
      else if (argument == "--manifest") {manifests.push_back(value);}
      else if (argument == "--plugin-dir") {directories.push_back(value);}
      else if (argument == "--plugin-index") {indexes.push_back(value);}
      else if (argument == "--state-file") {state_path = value;}
      else if (argument == "--control-socket") {control_path = value;}
      else {throw std::runtime_error("unknown option: " + argument);}
    }
    auto loader = std::make_shared<uran_platform_native::NativePluginLoader>();
    if (!config_path.empty()) {
      const auto config = YAML::LoadFile(config_path);
      if (config["plugin_manifests"]) {
        for (const auto & item : config["plugin_manifests"]) {
          manifests.push_back((uran_fs::absolute(config_path).parent_path() /
            item.as<std::string>()).string());
        }
      }
    }
    for (const auto & index : indexes) {
      std::ifstream input(index);
      const auto catalog = Json::parse(input);
      if (catalog.at("schema_version") != 1) {throw std::runtime_error("unsupported plugin index");}
      for (const auto & item : catalog.at("packages").items()) {
        manifests.push_back(item.value().at("manifest").get<std::string>());
      }
    }
    for (const auto & directory : directories) {
      for (const auto & entry : uran_fs::recursive_directory_iterator(directory)) {
        // GCC 7's experimental filesystem directory_entry does not expose
        // is_regular_file(); use the namespace-level query for both the
        // std::filesystem and experimental implementations.
        if (uran_fs::is_regular_file(entry.path()) && entry.path().filename() == "plugin.json") {
          manifests.push_back(entry.path().string());
        }
      }
    }
    for (const auto & manifest : manifests) {
      const auto result = loader->add_manifest(manifest);
      if (!result.ok) {throw std::runtime_error(result.message);}
    }
    auto platform = std::make_shared<uran_platform_native::NativePlatform>();
    std::shared_ptr<uran_plugin_api::StateStorage> storage;
    if (!state_path.empty()) {storage = std::make_shared<uran_platform_native::FileStateStorage>(state_path);}
    uran_core_framework::UranRuntime runtime(loader, platform, storage);
    runtime.set_unhandled_handler([](const auto & message) {
      auto payload = Json::parse(message.payload, nullptr, false);
      output({{"event", "downlink"}, {"message_type", message.message_type},
        {"payload", payload.is_discarded() ? Json(message.payload) : payload},
        {"correlation_id", message.correlation_id},
        {"source", {{"instance_id", message.source.instance_id},
        {"device_id", message.source.device_id}, {"component_id", message.source.component_id}}}});
    });
    const auto started = config_path.empty() ? runtime.start(YAML::Load("plugins: []")) : runtime.start_file(config_path);
    if (!started.ok) {throw std::runtime_error(started.message);}
    std::signal(SIGINT, stop_signal);
    std::signal(SIGTERM, stop_signal);
    if (!control_path.empty()) {
      ControlSocket server(control_path);
      output({{"event", "ready"}, {"control_socket", control_path}});
      while (!stopping) {server.serve(runtime, loader);}
    } else {
      std::string line;
      while (!stopping && std::getline(std::cin, line)) {
        try {output(command(Json::parse(line), runtime, loader));}
        catch (const std::exception & exception) {output({{"ok", false}, {"message", exception.what()}});}
      }
    }
    const auto stopped = runtime.stop();
    if (!stopped.ok) {std::cerr << stopped.message << '\n'; return 1;}
    return 0;
  } catch (const std::exception & exception) {std::cerr << exception.what() << '\n'; return 1;}
}
