#pragma once
#include <mutex>
#include "uran_plugin_api/state_storage.hpp"

namespace uran_platform_native
{
/** One host owns a state file. Atomic snapshots survive orderly restart. */
class FileStateStorage final : public uran_plugin_api::StateStorage
{
public:
  explicit FileStateStorage(std::string path);
  std::vector<uran_plugin_api::StoredState> load() override;
  uran_plugin_api::Status save(const std::vector<uran_plugin_api::StoredState> &) override;
private:
  std::string path_;
  std::mutex mutex_;
};
}
