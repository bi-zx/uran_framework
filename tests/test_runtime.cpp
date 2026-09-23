#include <atomic>
#include <future>
#include <map>
#include <mutex>
#include <thread>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "uran_core_framework/runtime.hpp"
#include "uran_core_framework/scoped_platform_services.hpp"
#include "uran_plugin_api/message_plugin.hpp"
#include "uran_platform_native/native_platform.hpp"
#include "uran_platform_native/native_plugin_loader.hpp"

using namespace std::chrono_literals;
namespace
{
using namespace uran_plugin_api;
using uran_core_framework::UranRuntime;
using Json = nlohmann::json;
struct Probe
{
  int configured{0}, activated{0}, stopped{0}, cleaned{0}, handled{0};
  std::string owner;
  std::string last_payload;
  std::string config_marker;
  std::shared_ptr<StateAccess> access;
  std::shared_ptr<PlatformServices> platform;
  std::shared_ptr<MessageAccess> messages;
  std::shared_ptr<CapabilityAccess> capabilities;
  std::function<void()> on_handle;
  bool fail_activate{false}, fail_cleanup{false}, fail_bind{false};
};
class Extension : public MessagePlugin
{
public:
  explicit Extension(std::shared_ptr<Probe> probe) : probe_(std::move(probe)) {}
  PluginDescriptor descriptor() const override {return {"extension", "2.1.0", kPluginApiVersion, "test/Extension"};}
  Status configure(const PluginContext & context, const YAML::Node & config) override
  {
    ++probe_->configured;
    probe_->owner = context.instance_id();
    probe_->config_marker = config["marker"] ? config["marker"].as<std::string>() : "";
    probe_->access = context.state_access();
    probe_->platform = context.platform();
    probe_->messages = context.messages();
    probe_->capabilities = context.capabilities();
    probe_->fail_activate = config["fail_activate"] && config["fail_activate"].as<bool>();
    probe_->fail_bind = config["fail_bind"] && config["fail_bind"].as<bool>();
    const bool persistent = config["persistent"] && config["persistent"].as<bool>();
    auto result = probe_->access->register_field({"plugin." + probe_->owner + ".counter", "int", "", "", persistent});
    if (result.ok) {result = probe_->access->set("plugin." + probe_->owner + ".counter", "0", probe_->owner);}
    return result;
  }
  Status activate() override
  {
    ++probe_->activated;
    return probe_->fail_activate ? Status::failure("activation failed") : Status::success();
  }
  Status deactivate() override {++probe_->stopped; return Status::success();}
  Status cleanup() override
  {
    ++probe_->cleaned;
    // Deliberately leave state registration behind; Runtime owns reclamation.
    return probe_->fail_cleanup ? Status::failure("cleanup failed") : Status::success();
  }
  PluginHealth health() const override {return {};}
  PluginCapabilities capabilities() const override {return {{"framework.echo"}, {"scheduler.serial"}, {}};}
  std::vector<std::string> message_types() const override
  {return probe_->fail_bind ? std::vector<std::string>{"echo", "echo"} : std::vector<std::string>{"echo"};}
  Status handle(const DownlinkEnvelope & message) override
  {
    EXPECT_TRUE(probe_->platform->on_executor_thread());
    ++probe_->handled;
    probe_->last_payload = message.payload;
    if (probe_->on_handle) {probe_->on_handle();}
    return Status::success();
  }
private:
  std::shared_ptr<Probe> probe_;
};
class Factories : public PluginLoader
{
public:
  std::vector<std::string> available_classes() const override {return {"test/Extension"};}
  std::shared_ptr<Plugin> create(const std::string & name) override
  {
    if (name != "test/Extension") {throw std::runtime_error("unknown class");}
    auto probe = std::make_shared<Probe>();
    probes.push_back(probe);
    return std::make_shared<Extension>(probe);
  }
  std::vector<std::shared_ptr<Probe>> probes;
};
class RuntimeStorage : public StateStorage
{
public:
  std::vector<StoredState> load() override {return records;}
  Status save(const std::vector<StoredState> & values) override
  {
    if (fail) {return Status::failure("storage unavailable");}
    records = values; ++saves; return Status::success();
  }
  std::vector<StoredState> records;
  int saves{0};
  bool fail{false};
};
YAML::Node specification(const std::string & id, const std::string & device = "robot_a", int priority = 0)
{
  auto node = YAML::Load("component_id: extension\nclass_type: test/Extension\nconfig: {}\n");
  node["instance_id"] = id;
  node["device_id"] = device;
  node["priority"] = priority;
  return node;
}
struct RuntimeTest : ::testing::Test
{
  std::shared_ptr<Factories> loader = std::make_shared<Factories>();
  std::shared_ptr<uran_platform_native::NativePlatform> platform = std::make_shared<uran_platform_native::NativePlatform>();
  UranRuntime runtime{loader, platform};
  void SetUp() override {ASSERT_TRUE(runtime.start(YAML::Load("plugins: []")).ok);}
};
}

TEST_F(RuntimeTest, AssemblySortingPreservesEachPrivateConfiguration)
{
  ASSERT_TRUE(runtime.stop().ok);
  auto assembly = YAML::Load("plugins: []");
  for (const auto & id : {"low", "high", "middle"}) {
    auto item = specification(id, "robot_a", std::string(id) == "high" ? 100 : 0);
    item["config"]["marker"] = id;
    assembly["plugins"].push_back(item);
  }
  ASSERT_TRUE(runtime.start(assembly).ok);
  ASSERT_EQ(loader->probes.size(), 3U);
  EXPECT_EQ(loader->probes.front()->owner, "high");
  for (const auto & probe : loader->probes) {EXPECT_EQ(probe->config_marker, probe->owner);}
  EXPECT_EQ(assembly["plugins"][0]["config"]["marker"].as<std::string>(), "low");
}

TEST_F(RuntimeTest, TargetRoutingAndLifecycleStaySynchronized)
{
  ASSERT_TRUE(runtime.load(specification("primary", "robot_a", 100)).ok);
  ASSERT_TRUE(runtime.load(specification("backup", "robot_a", 50)).ok);
  ASSERT_TRUE(runtime.load(specification("other", "robot_b", 200)).ok);
  EXPECT_EQ(runtime.providers("framework.echo", "robot_a").size(), 2U);
  DownlinkEnvelope command;
  command.message_type = "echo";
  command.payload = "payload";
  command.target = {"robot_a", "extension", "", "framework.echo"};
  ASSERT_TRUE(runtime.dispatch(command).ok);
  EXPECT_EQ(loader->probes[0]->handled, 1);
  EXPECT_EQ(loader->probes[2]->handled, 0);
  ASSERT_TRUE(runtime.deactivate("primary").ok);
  ASSERT_TRUE(runtime.dispatch(command).ok);
  EXPECT_EQ(loader->probes[1]->handled, 1);
  EXPECT_EQ(runtime.providers("framework.echo", "robot_a").size(), 1U);
  ASSERT_TRUE(runtime.activate("primary").ok);
  ASSERT_TRUE(runtime.unload("primary").ok);
  EXPECT_FALSE(Json::parse(runtime.state_snapshot()).contains("plugin.primary.counter"));
  EXPECT_FALSE(loader->probes[0]->access->set("plugin.primary.counter", "2", "primary").ok);
  EXPECT_THROW(loader->probes[0]->platform->post([] {}), std::runtime_error);
  EXPECT_EQ(runtime.providers("framework.echo", "robot_a").size(), 1U);
}
TEST_F(RuntimeTest, FailedLoadReclaimsStateCapabilitiesAndRoutes)
{
  auto bad = specification("bad");
  bad["config"]["fail_activate"] = true;
  EXPECT_FALSE(runtime.load(bad).ok);
  EXPECT_FALSE(Json::parse(runtime.state_snapshot()).contains("plugin.bad.counter"));
  EXPECT_TRUE(runtime.providers("framework.echo").empty());
  EXPECT_TRUE(Json::parse(runtime.diagnostics())["plugins"].empty());
  bad["config"]["fail_activate"] = false;
  bad["config"]["fail_bind"] = true;
  EXPECT_FALSE(runtime.load(bad).ok);
  EXPECT_TRUE(runtime.providers("framework.echo").empty());
  EXPECT_FALSE(Json::parse(runtime.state_snapshot()).contains("plugin.bad.counter"));
  ASSERT_TRUE(runtime.load(specification("bad")).ok);
}
TEST_F(RuntimeTest, CleanupFailureRemainsVisibleAndCanBeRetried)
{
  ASSERT_TRUE(runtime.load(specification("extension")).ok);
  loader->probes[0]->fail_cleanup = true;
  EXPECT_FALSE(runtime.unload("extension").ok);
  EXPECT_TRUE(runtime.providers("framework.echo").empty());
  auto diagnostic = Json::parse(runtime.diagnostics());
  ASSERT_EQ(diagnostic["plugins"].size(), 1U);
  EXPECT_EQ(diagnostic["plugins"][0]["state"], "error");
  loader->probes[0]->fail_cleanup = false;
  ASSERT_TRUE(runtime.unload("extension").ok);
  EXPECT_TRUE(Json::parse(runtime.diagnostics())["plugins"].empty());
}
TEST_F(RuntimeTest, ConcurrentAdministrativeRequestsAreSerialized)
{
  std::atomic<int> successful{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&, i] {
      if (runtime.load(specification("instance_" + std::to_string(i))).ok) {++successful;}
    });
  }
  for (auto & thread : threads) {thread.join();}
  EXPECT_EQ(successful, 8);
  EXPECT_EQ(runtime.providers("framework.echo").size(), 8U);
  EXPECT_FALSE(runtime.write_state("system.ready", "false", "uran_runtime").ok);
  EXPECT_FALSE(runtime.write_state("system.ready", "false", "external.native").ok);
}
TEST_F(RuntimeTest, CallbackCannotUnloadItselfOrStopTheRuntime)
{
  ASSERT_TRUE(runtime.load(specification("extension")).ok);
  auto probe = loader->probes.front();
  probe->on_handle = [&] {
    EXPECT_FALSE(runtime.unload("extension").ok);
    EXPECT_FALSE(runtime.stop().ok);
  };
  DownlinkEnvelope message{"echo", "{}", "request"};
  message.target.instance_id = "extension";
  EXPECT_TRUE(runtime.dispatch(message).ok);
  EXPECT_EQ(probe->cleaned, 0);
  EXPECT_EQ(probe->capabilities->providers("framework.echo", "robot_a").size(), 1U);
  ASSERT_TRUE(runtime.deactivate("extension").ok);
  const auto disabled = probe->messages->publish({"echo", "{}", "", false});
  EXPECT_FALSE(disabled.ok);
  EXPECT_EQ(disabled.message, "instance is not active");
  EXPECT_TRUE(runtime.unload("extension").ok);
}
TEST(RuntimeIntegration, StartupDurabilityCommitsOnlyAfterAssemblySucceeds)
{
  auto loader = std::make_shared<Factories>();
  auto platform = std::make_shared<uran_platform_native::NativePlatform>();
  auto storage = std::make_shared<RuntimeStorage>();
  UranRuntime runtime(loader, platform, storage);
  auto config = YAML::Load("plugins: []");
  auto good = specification("durable"); good["config"]["persistent"] = true;
  config["plugins"].push_back(good);
  auto bad = specification("bad"); bad["class_type"] = "missing/Plugin";
  config["plugins"].push_back(bad);
  EXPECT_FALSE(runtime.start(config).ok);
  EXPECT_EQ(storage->saves, 0);
  EXPECT_TRUE(storage->records.empty());
  config["plugins"].remove(1);
  storage->fail = true;
  EXPECT_FALSE(runtime.start(config).ok);
  EXPECT_EQ(runtime.state_snapshot(), "{}");
  storage->fail = false;
  ASSERT_TRUE(runtime.start(config).ok);
  EXPECT_EQ(storage->saves, 1);
  ASSERT_TRUE(runtime.write_state("ext.host", "7", "external.test", true).ok);
  EXPECT_EQ(storage->saves, 2);
}
TEST_F(RuntimeTest, StartupRollbackAndRestart)
{
  ASSERT_TRUE(runtime.stop().ok);
  auto assembly = YAML::Load("plugins: []");
  assembly["plugins"].push_back(specification("good"));
  auto bad = specification("bad"); bad["class_type"] = "missing/Plugin";
  assembly["plugins"].push_back(bad);
  EXPECT_FALSE(runtime.start(assembly).ok);
  EXPECT_FALSE(Json::parse(runtime.diagnostics())["ready"].get<bool>());
  EXPECT_EQ(runtime.state_snapshot(), "{}");
  ASSERT_TRUE(runtime.start(YAML::Load("plugins: []")).ok);
  ASSERT_TRUE(runtime.load(specification("good")).ok);
}
TEST(RuntimeIntegration, RealLibraryMultiInstanceFallbackAndSourceIdentity)
{
  auto loader = std::make_shared<uran_platform_native::NativePluginLoader>();
  ASSERT_TRUE(loader->add_manifest(URAN_TEST_MANIFEST).ok);
  auto platform = std::make_shared<uran_platform_native::NativePlatform>();
  UranRuntime runtime(loader, platform);
  auto config = YAML::Load("default_device_id: robot_a\nplugins: []");
  for (const auto & id : {"primary", "backup"}) {
    auto item = specification(id);
    item["class_type"] = "uran_core_example_plugins/LoopbackProtocolPlugin";
    item["priority"] = std::string(id) == "primary" ? 100 : 50;
    config["plugins"].push_back(item);
  }
  ASSERT_TRUE(runtime.start(config).ok);
  std::promise<DownlinkEnvelope> received;
  runtime.set_unhandled_handler([&](const auto & message) {received.set_value(message);});
  ASSERT_TRUE(runtime.deactivate("primary").ok);
  EXPECT_EQ(Json::parse(runtime.diagnostics())["primary_protocol_instance"], "backup");
  ASSERT_TRUE(runtime.publish({"echo", "{}", "req", false}, "primary").ok);
  auto future = received.get_future();
  ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
  const auto message = future.get();
  EXPECT_EQ(message.source.instance_id, "backup");
  EXPECT_EQ(message.source.device_id, "robot_a");
  EXPECT_EQ(message.correlation_id, "req");
  ASSERT_TRUE(runtime.unload("backup").ok);
  EXPECT_FALSE(runtime.publish({"echo", "{}", "", false}).ok);
  ASSERT_TRUE(runtime.activate("primary").ok);
  EXPECT_EQ(Json::parse(runtime.diagnostics())["protocol_instance_count"], 1);
  int obsolete_deliveries = 0;
  runtime.set_unhandled_handler([&](const auto &) {++obsolete_deliveries;});
  std::promise<void> reactivated;
  platform->post([&] {
    EXPECT_TRUE(runtime.publish({"echo", "{}", "obsolete", false}).ok);
    EXPECT_TRUE(runtime.deactivate("primary").ok);
    EXPECT_TRUE(runtime.activate("primary").ok);
    reactivated.set_value();
  });
  reactivated.get_future().get();
  runtime.diagnostics(); // Executor barrier after the queued downlink.
  EXPECT_EQ(obsolete_deliveries, 0);
  runtime.set_unhandled_handler(nullptr);
  ASSERT_TRUE(runtime.stop().ok);
}

TEST(RuntimeAccess, PinsPluginUplinkIdentityAndRevokesOnDeactivation)
{
  class Capture : public MessageAccess
  {
  public:
    Status publish(const UplinkEnvelope & message, const std::string &) override
    {source = message.source; ++calls; return Status::success();}
    PluginIdentity source;
    int calls{0};
  };
  auto capture = std::make_shared<Capture>();
  auto platform = std::make_shared<uran_platform_native::NativePlatform>();
  auto scope = std::make_shared<uran_core_framework::ScopedPlatformServices>(platform, "extension");
  const PluginIdentity identity{"extension", "robot_a", "extension"};
  uran_core_framework::ScopedMessageAccess access(capture, identity, scope);
  UplinkEnvelope forged{"event", "{}", "", false};
  forged.source = {"other", "robot_b", "communication"};
  ASSERT_TRUE(access.publish(forged).ok);
  EXPECT_EQ(capture->source, identity);
  scope->revoke();
  EXPECT_FALSE(access.publish(forged).ok);
  EXPECT_EQ(capture->calls, 1);
}
