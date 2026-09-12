#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include "uran_core_framework/state_store.hpp"
#include "uran_platform_native/file_state_storage.hpp"
using namespace uran_plugin_api;
using uran_core_framework::StateStore;
namespace
{
class MemoryStorage : public StateStorage
{
public:
  std::vector<StoredState> load() override {return values;}
  Status save(const std::vector<StoredState> & candidate) override
  {if (fail) {return Status::failure("disk full");} values = candidate; return Status::success();}
  bool fail{false};
  std::vector<StoredState> values;
};
}
TEST(StatePersistence, FailedWritesDoNotClaimFieldsOrNotify)
{
  auto storage = std::make_shared<MemoryStorage>();
  StateStore state(true, storage);
  int changes = 0;
  state.subscribe([&](const auto &, const auto &) {++changes;});
  storage->fail = true;
  EXPECT_FALSE(state.set("ext.setting", "1", "owner_a", true).ok);
  EXPECT_FALSE(state.contains("ext.setting"));
  EXPECT_EQ(changes, 0);
  storage->fail = false;
  ASSERT_TRUE(state.set("ext.setting", "2", "owner_b", true).ok);
  storage->fail = true;
  EXPECT_FALSE(state.set("ext.setting", "3", "owner_b", true).ok);
  StateValue value;
  ASSERT_TRUE(state.get("ext.setting", value));
  EXPECT_EQ(value.value_json, "2");
  EXPECT_EQ(changes, 1);
}
TEST(StatePersistence, RestoresTypesOwnershipAndValuesAcrossStoreLifetimes)
{
  auto storage = std::make_shared<MemoryStorage>();
  const StateFieldSpec field{"plugin.owner.setting", "object", "", "owner", true};
  {
    StateStore state(true, storage);
    ASSERT_TRUE(state.register_field(field).ok);
    ASSERT_TRUE(state.set(field.name, "{\"value\":42}", "owner").ok);
    ASSERT_TRUE(state.unregister_field(field.name, "owner").ok);
    ASSERT_TRUE(state.register_field(field).ok);
    EXPECT_EQ(state.snapshot_json(), "{\"plugin.owner.setting\":{\"value\":42}}");
  }
  StateStore restored(true, storage);
  ASSERT_TRUE(restored.register_field(field).ok);
  EXPECT_FALSE(restored.set(field.name, "{}", "another").ok);
  EXPECT_EQ(restored.snapshot_json(), "{\"plugin.owner.setting\":{\"value\":42}}");
}
TEST(StatePersistence, MissingBackendIsExplicitAndSignedRangeIsChecked)
{
  StateStore state;
  EXPECT_FALSE(state.set("ext.setting", "1", "owner", true).ok);
  EXPECT_FALSE(state.contains("ext.setting"));
  ASSERT_TRUE(state.register_field({"ext.signed", "int64", "", "owner", false}).ok);
  EXPECT_FALSE(state.set("ext.signed", "18446744073709551615", "owner").ok);
  EXPECT_TRUE(state.set("ext.signed", "9223372036854775807", "owner").ok);
}
TEST(StatePersistence, NativeFileRoundtripAndCorruptionIsReported)
{
  const auto directory = std::filesystem::temp_directory_path() /
    ("uran-state-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(directory);
  const auto path = directory / "state.json";
  {
    auto storage = std::make_shared<uran_platform_native::FileStateStorage>(path.string());
    StateStore state(true, storage);
    ASSERT_TRUE(state.set("ext.setting", "[1,true]", "owner", true).ok);
  }
  {
    auto storage = std::make_shared<uran_platform_native::FileStateStorage>(path.string());
    StateStore state(true, storage);
    EXPECT_EQ(state.snapshot_json(), "{\"ext.setting\":[1,true]}");
  }
  {std::ofstream output(path); output << "invalid json";}
  auto storage = std::make_shared<uran_platform_native::FileStateStorage>(path.string());
  EXPECT_THROW(StateStore(true, storage), std::exception);
  std::filesystem::remove_all(directory);
}
