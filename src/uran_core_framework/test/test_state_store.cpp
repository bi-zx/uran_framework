#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "uran_core_framework/scoped_state_access.hpp"
#include "uran_core_framework/state_store.hpp"

namespace uran_core_framework
{

TEST(StateStore, RegisteredFieldCanOnlyBeWrittenByOwner)
{
  StateStore store(false);
  const StateFieldSpec specification{
    "system.ready", "bool", "", "uran_runtime", false};

  ASSERT_TRUE(store.register_field(specification).ok);
  EXPECT_TRUE(store.set("system.ready", "true", "uran_runtime").ok);
  EXPECT_FALSE(store.set("system.ready", "false", "other_module").ok);

  StateValue value;
  ASSERT_TRUE(store.get("system.ready", value));
  EXPECT_EQ(value.value_json, "true");
  EXPECT_EQ(value.source, "uran_runtime");
  EXPECT_FALSE(store.set("system.ready", "1", "uran_runtime").ok);
}

TEST(ScopedStateAccess, RejectsImpersonatedOwner)
{
  auto store = std::make_shared<StateStore>(true);
  ScopedStateAccess access(store, "example_plugin");

  ASSERT_TRUE(access.register_field(
      StateFieldSpec{"plugin.example.ready", "bool", "", "", false}).ok);
  EXPECT_TRUE(access.set(
      "plugin.example.ready", "true", "example_plugin").ok);
  EXPECT_FALSE(access.set(
      "plugin.example.ready", "false", "another_plugin").ok);
  EXPECT_FALSE(access.register_field(
      StateFieldSpec{
        "plugin.other.ready", "bool", "", "another_plugin", false}).ok);
  EXPECT_FALSE(access.unregister_field(
      "plugin.example.ready", "another_plugin").ok);
  EXPECT_TRUE(access.unregister_field(
      "plugin.example.ready", "example_plugin").ok);
}

TEST(StateStore, InvalidJsonIsRejected)
{
  StateStore store(true);
  EXPECT_FALSE(store.set("ext.demo.value", "{invalid}", "demo").ok);
  EXPECT_TRUE(store.set("ext.demo.value", R"({"valid":true})", "demo").ok);
}

TEST(StateStore, ExtensionFieldClaimsOwnerOnFirstWrite)
{
  StateStore store(true);

  ASSERT_TRUE(store.set("ext.demo.value", "1", "plugin_a").ok);
  EXPECT_FALSE(store.set("ext.demo.value", "2", "plugin_b").ok);
  EXPECT_TRUE(store.set("ext.demo.value", "3", "plugin_a").ok);

  StateValue value;
  ASSERT_TRUE(store.get("ext.demo.value", value));
  EXPECT_EQ(value.value_json, "3");
  EXPECT_EQ(value.source, "plugin_a");
}

TEST(StateStore, RegisteredFieldDefinitionIsImmutable)
{
  StateStore store(false);
  const StateFieldSpec original{"system.ready", "bool", "", "runtime", false};

  ASSERT_TRUE(store.register_field(original).ok);
  EXPECT_TRUE(store.register_field(original).ok);
  EXPECT_FALSE(store.register_field(
      StateFieldSpec{"system.ready", "string", "", "runtime", false}).ok);
}

TEST(StateStore, ChangeCallbackCanReadStoreAgain)
{
  StateStore store(true);
  std::atomic<int> callback_count{0};

  const auto subscription = store.subscribe(
    [&store, &callback_count](const std::string & field_name, const StateValue &) {
      StateValue current;
      if (store.get(field_name, current)) {
        ++callback_count;
      }
    });

  ASSERT_NE(subscription, 0U);
  ASSERT_TRUE(store.set("ext.demo.counter", "1", "demo").ok);
  EXPECT_EQ(callback_count.load(), 1);

  store.unsubscribe(subscription);
  ASSERT_TRUE(store.set("ext.demo.counter", "2", "demo").ok);
  EXPECT_EQ(callback_count.load(), 1);
}

TEST(StateStore, FailingCallbackDoesNotInterruptOtherObservers)
{
  StateStore store(true);
  std::atomic<int> callback_count{0};

  ASSERT_NE(store.subscribe(
      [](const std::string &, const StateValue &) {
        throw std::runtime_error("模拟观察者异常");
      }), 0U);
  ASSERT_NE(store.subscribe(
      [&callback_count](const std::string &, const StateValue &) {
        ++callback_count;
      }), 0U);

  EXPECT_NO_THROW({
    EXPECT_TRUE(store.set("ext.demo.value", "1", "demo").ok);
  });
  EXPECT_EQ(callback_count.load(), 1);
}

TEST(StateStore, SnapshotPreservesJsonTypes)
{
  StateStore store(true);
  ASSERT_TRUE(store.set("ext.demo.boolean", "true", "demo").ok);
  ASSERT_TRUE(store.set("ext.demo.object", R"({"count":2})", "demo").ok);

  EXPECT_EQ(
    store.snapshot_json(),
    R"({"ext.demo.boolean":true,"ext.demo.object":{"count":2}})");
}

TEST(StateStore, OnlyOwnerCanUnregisterField)
{
  StateStore store;
  ASSERT_TRUE(store.register_field(
      StateFieldSpec{"plugin.camera.ready", "bool", "", "camera_main", false}).ok);
  ASSERT_TRUE(store.set(
      "plugin.camera.ready", "true", "camera_main").ok);

  EXPECT_FALSE(store.unregister_field(
      "plugin.camera.ready", "other_instance").ok);
  EXPECT_TRUE(store.contains("plugin.camera.ready"));
  EXPECT_TRUE(store.unregister_field(
      "plugin.camera.ready", "camera_main").ok);
  EXPECT_FALSE(store.contains("plugin.camera.ready"));
}

}
