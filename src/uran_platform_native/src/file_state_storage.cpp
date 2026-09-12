#include "uran_platform_native/file_state_storage.hpp"

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <unistd.h>

namespace uran_platform_native
{
using Json = nlohmann::json;
FileStateStorage::FileStateStorage(std::string path) : path_(std::move(path))
{if (path_.empty()) {throw std::invalid_argument("state file path is required");}}

std::vector<uran_plugin_api::StoredState> FileStateStorage::load()
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (!std::filesystem::exists(path_)) {return {};}
  std::ifstream input(path_);
  if (!input) {throw std::runtime_error("cannot read state file: " + path_);}
  const auto root = Json::parse(input);
  if (root.at("schema_version") != 1 || !root.at("fields").is_array()) {
    throw std::runtime_error("invalid state storage schema");
  }
  std::vector<uran_plugin_api::StoredState> result;
  for (const auto & field : root.at("fields")) {
    uran_plugin_api::StoredState record;
    record.specification = {field.at("name"), field.at("type"), field.at("unit"),
      field.at("owner"), field.at("declared_persistent")};
    record.value.value_json = field.at("value").dump();
    record.value.source = record.specification.owner;
    record.value.persistent = true;
    record.value.updated_at = std::chrono::system_clock::time_point(
      std::chrono::nanoseconds(field.at("updated_at_ns").get<std::int64_t>()));
    result.push_back(std::move(record));
  }
  return result;
}
uran_plugin_api::Status FileStateStorage::save(const std::vector<uran_plugin_api::StoredState> & records)
{
  std::lock_guard<std::mutex> lock(mutex_);
  std::string temporary;
  int descriptor = -1;
  try {
    Json root{{"schema_version", 1}, {"fields", Json::array()}};
    for (const auto & record : records) {
      const auto & spec = record.specification;
      root["fields"].push_back({{"name", spec.name}, {"type", spec.type}, {"unit", spec.unit},
        {"owner", spec.owner}, {"declared_persistent", spec.persistent},
        {"value", Json::parse(record.value.value_json)},
        {"updated_at_ns", std::chrono::duration_cast<std::chrono::nanoseconds>(
          record.value.updated_at.time_since_epoch()).count()}});
    }
    const auto path = std::filesystem::absolute(path_);
    std::filesystem::create_directories(path.parent_path());
    temporary = path.string() + ".tmp-XXXXXX";
    descriptor = mkstemp(temporary.data());
    if (descriptor < 0) {throw std::runtime_error("cannot create state snapshot");}
    const auto bytes = root.dump() + "\n";
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const auto written = write(descriptor, bytes.data() + offset, bytes.size() - offset);
      if (written < 0 && errno == EINTR) {continue;}
      if (written <= 0) {throw std::runtime_error("cannot write state snapshot");}
      offset += static_cast<std::size_t>(written);
    }
    if (fsync(descriptor) != 0) {throw std::runtime_error("cannot sync state snapshot");}
    close(descriptor); descriptor = -1;
    std::filesystem::rename(temporary, path);
    return uran_plugin_api::Status::success();
  } catch (const std::exception & exception) {
    if (descriptor >= 0) {close(descriptor);}
    if (!temporary.empty()) {unlink(temporary.c_str());}
    return uran_plugin_api::Status::failure(exception.what());
  }
}
}
