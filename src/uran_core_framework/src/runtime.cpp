#include "uran_core_framework/runtime.hpp"

#if __has_include(<filesystem>)
#include <filesystem>
namespace uran_fs = std::filesystem;
#else
#include <experimental/filesystem>
namespace uran_fs = std::experimental::filesystem;
#endif
#include <future>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <utility>
#include "uran_core_framework/config_manager.hpp"
#include "uran_core_framework/plugin_manager.hpp"
#include "uran_core_framework/protocol_gateway.hpp"
#include "uran_core_framework/route_catalog.hpp"
#include "uran_core_framework/state_store.hpp"
#include "uran_plugin_api/message_plugin.hpp"

namespace uran_core_framework
{
namespace
{
using uran_plugin_api::PlatformServices;
using uran_plugin_api::PluginContext;
using uran_plugin_api::PluginLoader;
using uran_plugin_api::DownlinkEnvelope;
using Json = nlohmann::json;

struct CallbackGuard
{
  explicit CallbackGuard(unsigned & depth) : depth_(depth) {++depth_;}
  ~CallbackGuard() {--depth_;}
  unsigned & depth_;
};

// Startup writes become durable only after the entire assembly is valid.
class StartupStorage final : public uran_plugin_api::StateStorage
{
public:
  explicit StartupStorage(std::shared_ptr<StateStorage> backend) : backend_(std::move(backend)) {}
  std::vector<uran_plugin_api::StoredState> load() override {return backend_->load();}
  Status save(const std::vector<uran_plugin_api::StoredState> & records) override
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (committed_) {return backend_->save(records);}
    pending_ = records;
    dirty_ = true;
    return Status::success();
  }
  Status commit()
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto saved = dirty_ ? backend_->save(pending_) : Status::success();
    if (saved.ok) {committed_ = true; pending_.clear();}
    return saved;
  }
private:
  std::shared_ptr<StateStorage> backend_;
  std::mutex mutex_;
  std::vector<uran_plugin_api::StoredState> pending_;
  bool dirty_{false}, committed_{false};
};

template<typename Function>
auto on_executor(const std::shared_ptr<PlatformServices> & platform, Function function)
  -> decltype(function())
{
  if (platform->on_executor_thread()) {return function();}
  auto task = std::make_shared<std::packaged_task<decltype(function())()>>(std::move(function));
  auto result = task->get_future();
  const auto pending = platform->post([task] {(*task)();});
  task.reset();
  return result.get();
}

class RuntimeMessageAccess final : public uran_plugin_api::MessageAccess
{
public:
  using Publish = std::function<Status(const uran_plugin_api::UplinkEnvelope &, const std::string &)>;
  explicit RuntimeMessageAccess(Publish publish) : publish_(std::move(publish)) {}
  Status publish(const uran_plugin_api::UplinkEnvelope & message, const std::string & preferred) override
  {return publish_(message, preferred);}
private:
  Publish publish_;
};

class RuntimeLocalMessageAccess final : public uran_plugin_api::LocalMessageAccess
{
public:
  using Dispatch = std::function<Status(const uran_plugin_api::DownlinkEnvelope &)>;
  explicit RuntimeLocalMessageAccess(Dispatch dispatch) : dispatch_(std::move(dispatch)) {}
  Status dispatch(const uran_plugin_api::DownlinkEnvelope & message) override
  {return dispatch_(message);}
private:
  Dispatch dispatch_;
};

struct Session
{
  std::shared_ptr<StateStore> state{std::make_shared<StateStore>()};
  std::shared_ptr<RouteCatalog> routes{std::make_shared<RouteCatalog>()};
  std::shared_ptr<CapabilityRegistry> capabilities{std::make_shared<CapabilityRegistry>()};
  std::shared_ptr<MessageRouter> router{std::make_shared<MessageRouter>()};
  std::shared_ptr<ProtocolGateway> gateway{std::make_shared<ProtocolGateway>()};
  std::unique_ptr<PluginManager> manager;
  std::unique_ptr<PluginContext> context;
  std::map<std::string, std::vector<std::size_t>> handler_ids;
  ConfigManager configuration;
  std::chrono::steady_clock::time_point started;

  ~Session()
  {
    gateway->set_downlink_handler(nullptr);
    for (const auto & id : gateway->protocol_instance_ids()) {
      static_cast<void>(gateway->unregister_protocol(id));
    }
    router->set_unhandled_handler(nullptr);
    for (const auto & item : handler_ids) {
      for (auto id : item.second) {router->unregister_handler(id);}
    }
    // Manager cleanup needs the context/state/platform to remain alive.
    manager.reset();
  }

  Status bind(const std::string & id)
  {
    for (const auto & active : manager->active_instances()) {
      if (active.identity.instance_id != id) {continue;}
      try {
        if (active.descriptor.plugin_type == "protocol") {
          const auto protocol = std::dynamic_pointer_cast<uran_plugin_api::ProtocolPlugin>(active.instance);
          if (!protocol) {return Status::failure("protocol plugin does not implement ProtocolPlugin");}
          const auto result = gateway->register_protocol(active.identity, protocol, active.priority);
          if (!result.ok) {return result;}
        }
        if (const auto handler = std::dynamic_pointer_cast<uran_plugin_api::MessagePlugin>(active.instance)) {
          const auto capabilities = handler->capabilities().features;
          const auto types = handler->message_types();
          if (types.empty() && active.descriptor.plugin_type != "protocol") {
            unbind(id); return Status::failure("message plugin declares no message types");
          }
          for (const auto & type : types) {
            std::size_t registration = 0;
            const std::weak_ptr<uran_plugin_api::MessagePlugin> weak_handler = handler;
            const auto registered = router->register_handler(
              MessageRoute{type, active.identity, capabilities, active.priority},
              [weak_handler](const DownlinkEnvelope & message) {
                const auto live = weak_handler.lock();
                return live ? live->handle(message) : Status::failure("handler unloaded");
              }, registration);
            if (!registered.ok) {unbind(id); return registered;}
            handler_ids[id].push_back(registration);
          }
        }
        return Status::success();
      } catch (const std::exception & exception) {unbind(id); return Status::failure(exception.what());}
      catch (...) {unbind(id); return Status::failure("binding plugin interfaces failed");}
    }
    return Status::failure("instance is not active: " + id);
  }

  void unbind(const std::string & id)
  {
    for (const auto & protocol : gateway->protocol_instance_ids()) {
      if (protocol == id) {static_cast<void>(gateway->unregister_protocol(id));}
    }
    const auto it = handler_ids.find(id);
    if (it != handler_ids.end()) {
      for (auto registration : it->second) {router->unregister_handler(registration);}
      handler_ids.erase(it);
    }
  }
};

const char * health_name(const uran_plugin_api::PluginHealth::Level level) noexcept
{
  using Level = uran_plugin_api::PluginHealth::Level;
  switch (level) {
    case Level::unknown: return "unknown";
    case Level::starting: return "starting";
    case Level::healthy: return "healthy";
    case Level::degraded: return "degraded";
    case Level::error: return "error";
    case Level::stopped: return "stopped";
  }
  return "unknown";
}

Json build_state_snapshot_document(const Session & session)
{
  Json document{{"schema_version", "1.0"}, {"timestamp_ns", static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count())},
    {"components", Json::array()}, {"unscoped_fields", Json::object()},
    {"fields", Json::object()}};

  const auto infos = session.manager ? session.manager->list() :
    std::vector<uran_plugin_api::PluginRuntimeInfo>{};
  std::map<std::pair<std::string, std::string>, std::size_t> component_indexes;
  std::map<std::string, std::pair<std::string, std::string>> instance_components;
  std::set<std::string> devices;

  for (const auto & info : infos) {
    const auto & identity = info.identity;
    if (identity.device_id.empty() || identity.component_id.empty() || identity.instance_id.empty()) {
      continue;
    }
    devices.insert(identity.device_id);
    const auto key = std::make_pair(identity.device_id, identity.component_id);
    auto found = component_indexes.find(key);
    if (found == component_indexes.end()) {
      const auto index = document["components"].size();
      component_indexes.emplace(key, index);
      document["components"].push_back({
          {"device_id", identity.device_id}, {"component_id", identity.component_id},
          {"instances", Json::array()}, {"fields", Json::object()}});
      found = component_indexes.find(key);
    }
    instance_components[identity.instance_id] = key;
    document["components"][found->second]["instances"].push_back({
      {"instance_id", identity.instance_id},
      {"status", uran_plugin_api::to_string(info.state)},
      {"health", health_name(info.health.level)},
      {"health_message", info.health.message},
      {"error", info.error},
      {"error_count", info.health.error_count},
      {"capabilities", info.capabilities.features}});
  }

  if (devices.size() == 1) {document["device_id"] = *devices.begin();}
  for (const auto & item : session.state->snapshot()) {
    const auto parsed = Json::parse(item.second.value_json, nullptr, false);
    const auto value = parsed.is_discarded() ? Json(item.second.value_json) : parsed;
    document["fields"][item.first] = value;
    const auto owner = instance_components.find(item.second.source);
    if (owner == instance_components.end()) {
      document["unscoped_fields"][item.first] = value;
      continue;
    }
    const auto component = component_indexes.find(owner->second);
    if (component != component_indexes.end()) {
      document["components"][component->second]["fields"][item.first] = value;
    }
  }
  return document;
}

class SessionRuntimeSnapshotAccess final : public uran_plugin_api::RuntimeSnapshotAccess
{
public:
  explicit SessionRuntimeSnapshotAccess(std::weak_ptr<Session> session)
  : session_(std::move(session)) {}

  std::string snapshot_json() const override
  {
    const auto session = session_.lock();
    return session ? build_state_snapshot_document(*session).dump() : "{}";
  }

private:
  std::weak_ptr<Session> session_;
};
}

struct UranRuntime::Impl
{
  std::shared_ptr<PluginLoader> loader;
  std::shared_ptr<PlatformServices> platform;
  std::shared_ptr<uran_plugin_api::StateStorage> storage;
  std::shared_ptr<Session> session;
  MessageRouter::Handler unhandled;
  std::shared_ptr<unsigned> callback_depth{std::make_shared<unsigned>(0)};

  template<typename Function>
  auto call(Function function) -> decltype(function())
  {
    return on_executor(platform, [this, &function] {
      CallbackGuard guard(*callback_depth);
      return function();
    });
  }

  template<typename Function>
  Status mutate(Function function)
  {
    try {
      return on_executor(platform, [this, &function] {
        if (*callback_depth) {return Status::failure("reentrant lifecycle operation rejected");}
        CallbackGuard guard(*callback_depth);
        return function();
      });
    } catch (const std::exception & exception) {return Status::failure(exception.what());}
    catch (...) {return Status::failure("runtime operation threw an unknown exception");}
  }

  Status load_into(const std::shared_ptr<Session> & target, const YAML::Node & item)
  {
    YAML::Node assembly = YAML::Clone(target->configuration.snapshot());
    assembly["plugins"] = YAML::Node(YAML::NodeType::Sequence);
    assembly["plugins"].push_back(YAML::Clone(item));
    const auto loaded = target->manager->configure_and_load_all(assembly, *target->context);
    if (!loaded.ok) {return loaded;}
    const auto id = item["instance_id"].as<std::string>();
    for (const auto & active : target->manager->active_instances()) {
      if (active.identity.instance_id == id) {
        const auto bound = target->bind(id);
        if (!bound.ok) {
          target->unbind(id);
          const auto rollback = target->manager->unload(id);
          return Status::failure(bound.message + (rollback.ok ? "" : "; rollback: " + rollback.message));
        }
      }
    }
    return Status::success();
  }
};

UranRuntime::UranRuntime(std::shared_ptr<PluginLoader> loader, std::shared_ptr<PlatformServices> platform,
  std::shared_ptr<uran_plugin_api::StateStorage> storage)
: impl_(std::make_unique<Impl>())
{
  if (!loader || !platform) {throw std::invalid_argument("loader and platform are required");}
  impl_->loader = std::move(loader);
  impl_->platform = std::move(platform);
  impl_->storage = std::move(storage);
}
UranRuntime::~UranRuntime()
{
  try {on_executor(impl_->platform, [this] {impl_->session.reset();});} catch (...) {}
}

Status UranRuntime::start(const YAML::Node & configuration)
{
  return impl_->mutate([this, &configuration] {
    if (impl_->session) {return Status::failure("runtime already started");}
    auto candidate = std::make_shared<Session>();
    const auto staged_storage = impl_->storage ? std::make_shared<StartupStorage>(impl_->storage) : nullptr;
    candidate->state = std::make_shared<StateStore>(true, staged_storage,
      [platform = impl_->platform] {return platform->system_now();});
    const auto configured = candidate->configuration.load_node(configuration);
    if (!configured.ok) {return configured;}
    const auto root = candidate->configuration.snapshot();
    if (root["routes"]) {
      const auto routes = candidate->routes->load_node(root["routes"]);
      if (!routes.ok) {return routes;}
    }
    candidate->manager = std::make_unique<PluginManager>(impl_->loader, candidate->capabilities);
    const std::weak_ptr<Session> weak_session = candidate;
    auto messages = std::make_shared<RuntimeMessageAccess>(
      [weak_session, depth = impl_->callback_depth](const auto & message, const auto & preferred) {
        CallbackGuard guard(*depth);
        const auto live = weak_session.lock();
        return live ? live->gateway->publish(message, preferred) : Status::failure("runtime stopped");
      });
    auto local_messages = std::make_shared<RuntimeLocalMessageAccess>(
      [weak_session, depth = impl_->callback_depth](const auto & message) {
        CallbackGuard guard(*depth);
        const auto live = weak_session.lock();
        return live ? live->router->route(message) : Status::failure("runtime stopped");
      });
    candidate->context = std::make_unique<PluginContext>(
      impl_->platform, candidate->state, candidate->routes,
      PluginIdentity{"uran_runtime", "runtime", "runtime"}, "runtime", messages,
      candidate->capabilities, local_messages, candidate->state,
      std::make_shared<SessionRuntimeSnapshotAccess>(candidate));
    candidate->started = impl_->platform->steady_now();
    candidate->router->set_unhandled_handler(impl_->unhandled);
    const std::weak_ptr<PlatformServices> weak_platform = impl_->platform;
    candidate->gateway->set_downlink_handler(
      [weak_session, weak_platform, depth = impl_->callback_depth](const PluginIdentity &, const DownlinkEnvelope & message) {
        if (const auto platform = weak_platform.lock()) {
          // Marshal network callbacks onto the same executor as lifecycle operations.
          platform->post([weak_session, message, depth] {
            CallbackGuard guard(*depth);
            if (const auto live = weak_session.lock()) {
              if (live->gateway->current_source(message.source, message.source_generation)) {
                static_cast<void>(live->router->route(message));
              }
            }
          });
        }
      });
    const auto registered = candidate->state->register_field({"system.ready", "bool", "", "uran_runtime", false});
    if (!registered.ok) {return registered;}
    const auto initializing = candidate->state->set("system.ready", "false", "uran_runtime");
    if (!initializing.ok) {return initializing;}
    const auto loaded = candidate->manager->configure_and_load_all(root, *candidate->context);
    if (!loaded.ok) {return loaded;}
    for (const auto & active : candidate->manager->active_instances()) {
      const auto bound = candidate->bind(active.identity.instance_id);
      if (!bound.ok) {return bound;}
    }
    if (root["primary_protocol_instance"]) {
      const auto primary = root["primary_protocol_instance"].as<std::string>();
      if (!primary.empty()) {
        const auto selected = candidate->gateway->set_primary_instance(primary);
        if (!selected.ok) {return selected;}
      }
    }
    const auto ready = candidate->state->set("system.ready", "true", "uran_runtime");
    if (!ready.ok) {return ready;}
    if (staged_storage) {
      const auto committed = staged_storage->commit();
      if (!committed.ok) {return committed;}
    }
    impl_->session = std::move(candidate);
    return Status::success("runtime started");
  });
}

Status UranRuntime::start_file(const std::string & path)
{
  try {
    const auto absolute = uran_fs::absolute(path);
    auto config = YAML::LoadFile(absolute.string());
    if (config["plugins"] && config["plugins"].IsSequence()) {
      for (auto item : config["plugins"]) {
        if (item["config_file"]) {
          item["config_file"] = (absolute.parent_path() /
            item["config_file"].as<std::string>()).string();
        }
      }
    }
    return start(config);
  } catch (const std::exception & exception) {return Status::failure(exception.what());}
}

Status UranRuntime::stop()
{
  return impl_->mutate([this] {
    if (!impl_->session) {return Status::success("runtime already stopped");}
    auto session = impl_->session;
    std::string errors;
    auto instances = session->manager->list();
    for (auto it = instances.rbegin(); it != instances.rend(); ++it) {
      session->unbind(it->identity.instance_id);
      const auto result = session->manager->unload(it->identity.instance_id);
      if (!result.ok) {errors += it->identity.instance_id + ": " + result.message + "; ";}
    }
    session->state->set("system.ready", "false", "uran_runtime");
    if (!errors.empty()) {return Status::failure(errors);}
    impl_->session.reset();
    return Status::success("runtime stopped");
  });
}
Status UranRuntime::load(const YAML::Node & specification)
{
  return impl_->mutate([this, &specification] {
    if (!impl_->session) {return Status::failure("runtime is not started");}
    return impl_->load_into(impl_->session, specification);
  });
}
Status UranRuntime::activate(const std::string & id)
{
  return impl_->mutate([this, &id] {
    const auto session = impl_->session;
    if (!session) {return Status::failure("runtime is not started");}
    const auto result = session->manager->activate(id);
    if (!result.ok) {return result;}
    const auto bound = session->bind(id);
    if (!bound.ok) {session->unbind(id); session->manager->deactivate(id);}
    return bound;
  });
}
Status UranRuntime::deactivate(const std::string & id)
{
  return impl_->mutate([this, &id] {
    if (!impl_->session) {return Status::failure("runtime is not started");}
    impl_->session->unbind(id);
    return impl_->session->manager->deactivate(id);
  });
}
Status UranRuntime::unload(const std::string & id)
{
  return impl_->mutate([this, &id] {
    if (!impl_->session) {return Status::failure("runtime is not started");}
    impl_->session->unbind(id);
    return impl_->session->manager->unload(id);
  });
}
Status UranRuntime::set_primary_protocol(const std::string & id)
{
  return impl_->mutate([this, &id] {
    return impl_->session ? impl_->session->gateway->set_primary_instance(id) :
      Status::failure("runtime is not started");
  });
}
Status UranRuntime::publish(const uran_plugin_api::UplinkEnvelope & message, const std::string & preferred)
{
  return impl_->call([this, &message, &preferred] {
    return impl_->session ? impl_->session->gateway->publish(message, preferred) :
      Status::failure("runtime is not started");
  });
}
Status UranRuntime::dispatch(const DownlinkEnvelope & message)
{
  return impl_->call([this, &message] {
    return impl_->session ? impl_->session->router->route(message) :
      Status::failure("runtime is not started");
  });
}
void UranRuntime::set_unhandled_handler(MessageRouter::Handler handler)
{
  impl_->call([this, &handler] {
    impl_->unhandled = std::move(handler);
    if (impl_->session) {impl_->session->router->set_unhandled_handler(impl_->unhandled);}
  });
}
Status UranRuntime::write_state(const std::string & field, const std::string & json,
  const std::string & source, bool persistent)
{
  return impl_->call([this, &field, &json, &source, persistent] {
    if (source.rfind("external.", 0) != 0) {return Status::failure("host source must start with external.");}
    return impl_->session ? impl_->session->state->set(field, json, source, persistent) :
      Status::failure("runtime is not started");
  });
}
std::string UranRuntime::state_snapshot() const
{
  return impl_->call([this] {
    return impl_->session ? impl_->session->state->snapshot_json() : std::string("{}");
  });
}

std::string UranRuntime::state_snapshot_document() const
{
  return impl_->call([this] {
    return impl_->session ? build_state_snapshot_document(*impl_->session) : Json::object();
  }).dump();
}

std::string UranRuntime::diagnostics() const
{
  return impl_->call([this] {
    Json result{{"ready", false}, {"api_version", uran_plugin_api::kPluginApiVersion},
      {"plugins", Json::array()}, {"capability_count", 0}, {"protocol_instance_count", 0}};
    if (const auto session = impl_->session) {
      StateValue ready;
      result["ready"] = session->state->get("system.ready", ready) && ready.value_json == "true";
      result["uptime_seconds"] = std::chrono::duration_cast<std::chrono::seconds>(
        impl_->platform->steady_now() - session->started).count();
      result["configuration_version"] = session->configuration.version();
      result["capability_count"] = session->capabilities->capability_count();
      result["protocol_instance_count"] = session->gateway->protocol_instance_ids().size();
      result["primary_protocol_instance"] = session->gateway->primary_instance_id();
      for (const auto & info : session->manager->list()) {
        result["plugins"].push_back({{"instance_id", info.identity.instance_id},
          {"device_id", info.identity.device_id}, {"component_id", info.identity.component_id},
          {"class_type", info.descriptor.class_type}, {"state", to_string(info.state)},
          {"health", static_cast<int>(info.health.level)}, {"health_message", info.health.message},
          {"error", info.error}, {"capabilities", info.capabilities.features}});
      }
    }
    return result.dump();
  });
}
std::vector<CapabilityProvider> UranRuntime::providers(const std::string & capability,
  const std::string & device, const std::string & component) const
{
  return impl_->call([this, &capability, &device, &component] {
    return impl_->session ? impl_->session->capabilities->providers(capability, device, component) :
      std::vector<CapabilityProvider>{};
  });
}
}
