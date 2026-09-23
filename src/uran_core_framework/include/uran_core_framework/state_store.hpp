#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "uran_core_framework/types.hpp"
#include "uran_core_framework/visibility_control.hpp"
#include "uran_plugin_api/runtime_access.hpp"
#include "uran_plugin_api/state_storage.hpp"

namespace uran_core_framework
{

struct ObservationRegistry;

/**
 * @brief 维护字段定义、所有权和 JSON 状态值的线程安全状态空间。
 *
 * 写入在锁内原子提交，变化回调复制后在锁外执行。
 */
class URAN_CORE_FRAMEWORK_PUBLIC StateStore final
  : public uran_plugin_api::StateAccess,
    public uran_plugin_api::StateObserverAccess
{
public:
  using ChangeCallback = std::function<void(
      const std::string &, const StateValue &)>;

  explicit StateStore(bool allow_extension_fields = true,
    std::shared_ptr<uran_plugin_api::StateStorage> storage = {},
    std::function<std::chrono::system_clock::time_point()> clock = std::chrono::system_clock::now);
  ~StateStore();

  StateStore(const StateStore &) = delete;
  StateStore & operator=(const StateStore &) = delete;

  Status register_field(const StateFieldSpec & specification) override;
  Status unregister_field(
    const std::string & field_name,
    const std::string & source) override;
  Status set(
    const std::string & field_name,
    const std::string & value_json,
    const std::string & source,
    bool persistent = false) override;

  bool contains(const std::string & field_name) const;
  bool get(const std::string & field_name, StateValue & value) const override;
  std::map<std::string, StateValue> snapshot() const override;
  std::string snapshot_json() const override;

  std::size_t subscribe(ChangeCallback callback);
  void unsubscribe(std::size_t subscription_id);
  std::shared_ptr<uran_plugin_api::StateObservation> observe(
    const std::string & field_prefix,
    uran_plugin_api::StateChangeCallback callback) override;

private:
  static bool valid_field_name(const std::string & field_name);
  static bool supported_type(const std::string & declared_type);
  static bool valid_json_value(const std::string & value_json);
  static bool valid_value_for_type(
    const std::string & value_json,
    const std::string & declared_type);

  bool allow_extension_fields_{true};
  std::shared_ptr<uran_plugin_api::StateStorage> storage_;
  std::function<std::chrono::system_clock::time_point()> clock_;
  std::map<std::string, uran_plugin_api::StoredState> durable_;
  mutable std::shared_mutex mutex_;
  std::map<std::string, StateFieldSpec> specifications_;
  std::map<std::string, StateValue> values_;
  std::shared_ptr<ObservationRegistry> observations_;
};

}
