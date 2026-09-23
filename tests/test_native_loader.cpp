#include <gtest/gtest.h>
#include "uran_platform_native/native_plugin_loader.hpp"
#include "uran_plugin_api/protocol_plugin.hpp"

TEST(NativeLoader, RetainsLibraryAfterLoaderDestruction)
{
  std::shared_ptr<uran_plugin_api::Plugin> plugin;
  {
    uran_platform_native::NativePluginLoader loader;
    ASSERT_TRUE(loader.add_manifest(URAN_TEST_MANIFEST).ok);
    plugin = loader.create("uran_core_example_plugins/LoopbackProtocolPlugin");
    EXPECT_THROW(loader.create("missing/class"), std::runtime_error);
  }
  EXPECT_EQ(plugin->descriptor().api_version, uran_plugin_api::kPluginApiVersion);
  EXPECT_NE(std::dynamic_pointer_cast<uran_plugin_api::ProtocolPlugin>(plugin), nullptr);
  plugin.reset();
}
