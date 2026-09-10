#include <cstdint>

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include "uran_core_framework/config_manager.hpp"

namespace uran_core_framework
{

TEST(ConfigManager, StoresIndependentSnapshotAndVersion)
{
  ConfigManager manager;
  auto source = YAML::Load(R"(
runtime:
  instance: robot_a
)");

  ASSERT_TRUE(manager.load_node(source).ok);
  EXPECT_EQ(manager.version(), 1U);
  EXPECT_TRUE(manager.source_path().empty());

  source["runtime"]["instance"] = "mutated";
  auto snapshot = manager.snapshot();
  EXPECT_EQ(snapshot["runtime"]["instance"].as<std::string>(), "robot_a");

  snapshot["runtime"]["instance"] = "local_change";
  EXPECT_EQ(
    manager.snapshot()["runtime"]["instance"].as<std::string>(),
    "robot_a");
}

TEST(ConfigManager, InvalidRootDoesNotReplaceCurrentSnapshot)
{
  ConfigManager manager;
  ASSERT_TRUE(manager.load_node(YAML::Load("runtime: {enabled: true}")).ok);

  EXPECT_FALSE(manager.load_node(YAML::Load("[invalid, root]")).ok);
  EXPECT_EQ(manager.version(), 1U);
  EXPECT_TRUE(manager.snapshot()["runtime"]["enabled"].as<bool>());
}

}
