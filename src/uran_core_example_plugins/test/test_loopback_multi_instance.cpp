#include <memory>
#include <string>

#include <gtest/gtest.h>
#include <rclcpp/rclcpp.hpp>
#include <yaml-cpp/yaml.h>

#include "uran_core_example_plugins/loopback_protocol_plugin.hpp"
#include "uran_core_framework/route_catalog.hpp"
#include "uran_core_framework/scoped_state_access.hpp"
#include "uran_core_framework/state_store.hpp"
#include "uran_plugin_api/plugin_context.hpp"

namespace uran_core_example_plugins
{

class LoopbackMultiInstanceTest : public ::testing::Test
{
protected:
  static void SetUpTestSuite()
  {
    rclcpp::init(0, nullptr);
  }

  static void TearDownTestSuite()
  {
    rclcpp::shutdown();
  }
};

TEST_F(LoopbackMultiInstanceTest, KeepsConfigurationLifecycleAndStateIndependent)
{
  auto node = std::make_shared<rclcpp::Node>("loopback_multi_instance_test");
  auto state_store = std::make_shared<uran_core_framework::StateStore>(true);
  auto route_catalog = std::make_shared<uran_core_framework::RouteCatalog>();

  const uran_plugin_api::PluginIdentity primary_identity{
    "protocol_primary", "robot_001", "communication"};
  const uran_plugin_api::PluginIdentity backup_identity{
    "protocol_backup", "robot_001", "communication"};
  auto primary_state = std::make_shared<uran_core_framework::ScopedStateAccess>(
    state_store, primary_identity.instance_id);
  auto backup_state = std::make_shared<uran_core_framework::ScopedStateAccess>(
    state_store, backup_identity.instance_id);
  uran_plugin_api::PluginContext primary_context(
    node, primary_state, route_catalog, primary_identity, "/robot_001/communication");
  uran_plugin_api::PluginContext backup_context(
    node, backup_state, route_catalog, backup_identity, "/robot_001/communication");

  LoopbackProtocolPlugin primary;
  LoopbackProtocolPlugin backup;
  ASSERT_TRUE(primary.configure(
      primary_context, YAML::Load("{channel: primary}")).ok);
  ASSERT_TRUE(backup.configure(
      backup_context, YAML::Load("{channel: backup}")).ok);
  ASSERT_TRUE(primary.activate().ok);
  ASSERT_TRUE(backup.activate().ok);

  uran_plugin_api::StateValue primary_connected;
  uran_plugin_api::StateValue backup_connected;
  ASSERT_TRUE(state_store->get("plugin.protocol_primary.connected", primary_connected));
  ASSERT_TRUE(state_store->get("plugin.protocol_backup.connected", backup_connected));
  EXPECT_EQ(primary_connected.value_json, "true");
  EXPECT_EQ(backup_connected.value_json, "true");
  EXPECT_NE(
    primary.capabilities().provided_state_fields,
    backup.capabilities().provided_state_fields);

  ASSERT_TRUE(primary.deactivate().ok);
  ASSERT_TRUE(state_store->get("plugin.protocol_primary.connected", primary_connected));
  ASSERT_TRUE(state_store->get("plugin.protocol_backup.connected", backup_connected));
  EXPECT_EQ(primary_connected.value_json, "false");
  EXPECT_EQ(backup_connected.value_json, "true");

  ASSERT_TRUE(backup.deactivate().ok);
  EXPECT_TRUE(primary.cleanup().ok);
  EXPECT_TRUE(backup.cleanup().ok);
}

}
