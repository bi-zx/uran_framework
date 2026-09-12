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
  heartbeat: framework.heartbeat
  state_snapshot: framework.state_snapshot
)");

  ASSERT_TRUE(catalog.load_node(config).ok);
  std::string route;
  ASSERT_TRUE(catalog.resolve("heartbeat", route));
  EXPECT_EQ(route, "framework.heartbeat");
  EXPECT_FALSE(catalog.resolve("missing", route));
}

TEST(RouteCatalog, InvalidRouteValueDoesNotReplaceCurrentSnapshot)
{
  RouteCatalog catalog(std::map<std::string, std::string>{{"heartbeat", "original.message"}});
  const auto invalid = YAML::Load(R"(
routes:
  heartbeat:
    nested: value
)");

  EXPECT_FALSE(catalog.load_node(invalid).ok);
  std::string route;
  ASSERT_TRUE(catalog.resolve("heartbeat", route));
  EXPECT_EQ(route, "original.message");
}

}

TEST(RouteCatalogBoundary, RejectsPlatformEndpointNames)
{
  uran_core_framework::RouteCatalog catalog;
  EXPECT_FALSE(catalog.set("heartbeat", "/uran/runtime/heartbeat").ok);
  EXPECT_TRUE(catalog.set("heartbeat", "framework.heartbeat").ok);
}
