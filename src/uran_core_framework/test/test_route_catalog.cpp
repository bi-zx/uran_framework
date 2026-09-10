#include <string>

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include "uran_core_framework/route_catalog.hpp"

namespace uran_core_framework
{

TEST(RouteCatalog, LoadsRoutesFromNamedRoot)
{
  RouteCatalog catalog;
  const auto config = YAML::Load(R"(
routes:
  heartbeat: /uran/runtime/heartbeat
  state_snapshot: /uran/runtime/state_snapshot
)");

  ASSERT_TRUE(catalog.load_node(config).ok);
  std::string route;
  ASSERT_TRUE(catalog.resolve("heartbeat", route));
  EXPECT_EQ(route, "/uran/runtime/heartbeat");
  EXPECT_FALSE(catalog.resolve("missing", route));
}

TEST(RouteCatalog, InvalidRouteValueDoesNotReplaceCurrentSnapshot)
{
  RouteCatalog catalog({{"heartbeat", "/original"}});
  const auto invalid = YAML::Load(R"(
routes:
  heartbeat:
    nested: value
)");

  EXPECT_FALSE(catalog.load_node(invalid).ok);
  std::string route;
  ASSERT_TRUE(catalog.resolve("heartbeat", route));
  EXPECT_EQ(route, "/original");
}

}
