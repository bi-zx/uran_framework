#include "uran_core_framework/state_store.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <cstdint>
#include <limits>
#include <regex>
#include <utility>
#include <stdexcept>
#include <thread>

#include <nlohmann/json.hpp>

namespace uran_core_framework
{

struct ObservationRegistry
{
  struct Entry
  {
    std::mutex mutex;
    std::condition_variable idle;
    StateStore::ChangeCallback callback;
    bool cancelled{false};
    bool running{false};
    std::thread::id runner;
    std::deque<std::pair<std::string, StateValue>> pending;
  };

  std::mutex mutex;
  std::map<std::size_t, std::shared_ptr<Entry>> entries;
  std::size_t next_id{1};
  bool alive{true};

  std::size_t add(StateStore::ChangeCallback callback)
  {
    auto entry = std::make_shared<Entry>();
    entry->callback = std::move(callback);
    std::lock_guard<std::mutex> lock(mutex);
    if (!alive) {return 0;}
    const auto id = next_id++;
    entries.emplace(id, std::move(entry));
    return id;
  }

  void cancel(std::size_t id) noexcept
  {
    std::shared_ptr<Entry> entry;
    {
      std::lock_guard<std::mutex> lock(mutex);
      const auto found = entries.find(id);
      if (found == entries.end()) {return;}
      entry = found->second;
      entries.erase(found);
    }
    std::unique_lock<std::mutex> lock(entry->mutex);
    entry->cancelled = true;
    entry->callback = nullptr;
    entry->pending.clear();
    entry->idle.notify_all();
    if (entry->runner != std::this_thread::get_id()) {
      entry->idle.wait(lock, [&entry] {return !entry->running;});
    }
  }

  void emit(const std::string & field, const StateValue & value)
  {
    std::vector<std::shared_ptr<Entry>> copy;
    {
      std::lock_guard<std::mutex> lock(mutex);
      for (const auto & item : entries) {copy.push_back(item.second);}
    }
    for (const auto & entry : copy) {
      bool drain = false;
      {
        std::lock_guard<std::mutex> lock(entry->mutex);
        if (entry->cancelled || !entry->callback) {continue;}
        entry->pending.emplace_back(field, value);
        if (!entry->running) {
          entry->running = true;
          entry->runner = std::this_thread::get_id();
          drain = true;
        }
      }
      if (!drain) {continue;}
      while (true) {
        StateStore::ChangeCallback callback;
        std::pair<std::string, StateValue> change;
        {
          std::lock_guard<std::mutex> lock(entry->mutex);
          if (entry->cancelled || entry->pending.empty()) {
            entry->running = false;
            entry->runner = {};
            entry->idle.notify_all();
            break;
          }
          change = std::move(entry->pending.front());
          entry->pending.pop_front();
          callback = entry->callback;
        }
        try {if (callback) {callback(change.first, change.second);}} catch (...) {}
      }
    }
  }

  void shutdown() noexcept
  {
    std::vector<std::size_t> ids;
    {
      std::lock_guard<std::mutex> lock(mutex);
      alive = false;
      for (const auto & item : entries) {ids.push_back(item.first);}
    }
    for (const auto id : ids) {cancel(id);}
  }
};

namespace
{

class StoreObservation final : public uran_plugin_api::StateObservation
{
public:
  StoreObservation(std::weak_ptr<ObservationRegistry> registry, std::size_t id)
  : registry_(std::move(registry)), id_(id) {}
  ~StoreObservation() override {cancel();}
  void cancel() noexcept override
  {
    try {
      std::size_t id = 0;
      {std::lock_guard<std::mutex> lock(mutex_); std::swap(id, id_);}
      if (id) {if (const auto registry = registry_.lock()) {registry->cancel(id);}}
    } catch (...) {}
  }
private:
  std::mutex mutex_;
  std::weak_ptr<ObservationRegistry> registry_;
  std::size_t id_{0};
};

std::string trim_copy(const std::string & value)
{
  const auto begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return {};
  }
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1U);
}

}

StateStore::StateStore(const bool allow_extension_fields,
  std::shared_ptr<uran_plugin_api::StateStorage> storage,
  std::function<std::chrono::system_clock::time_point()> clock)
: allow_extension_fields_(allow_extension_fields), storage_(std::move(storage)), clock_(std::move(clock)),
  observations_(std::make_shared<ObservationRegistry>())
{
  if (!clock_) {throw std::invalid_argument("state clock is required");}
  if (storage_) {
    for (const auto & record : storage_->load()) {
      const auto & spec = record.specification;
      const auto & value = record.value;
      if (!valid_field_name(spec.name) || spec.owner.empty() || !supported_type(spec.type) ||
        value.source != spec.owner || !value.persistent || !valid_json_value(value.value_json) ||
        !valid_value_for_type(value.value_json, spec.type) || durable_.count(spec.name))
      {throw std::runtime_error("invalid persistent state record: " + spec.name);}
      durable_.emplace(spec.name, record);
      specifications_.emplace(spec.name, spec);
      values_.emplace(spec.name, value);
    }
  }
}

StateStore::~StateStore() {observations_->shutdown();}

Status StateStore::register_field(const StateFieldSpec & specification)
{
  if (!valid_field_name(specification.name)) {
    return Status::failure("状态字段名称不合法: " + specification.name);
  }
  if (specification.owner.empty()) {
    return Status::failure("状态字段必须声明所有者: " + specification.name);
  }
  if (!supported_type(specification.type)) {
    return Status::failure(
      "状态字段类型不受支持: " + specification.name +
      "，类型=" + specification.type);
  }

  std::unique_lock<std::shared_mutex> lock(mutex_);
  const auto iterator = specifications_.find(specification.name);
  if (iterator != specifications_.end()) {
    if (iterator->second.owner != specification.owner) {
      return Status::failure(
        "状态字段已由其他模块注册: " + specification.name);
    }
    const auto & current = iterator->second;
    const bool unchanged =
      current.type == specification.type &&
      current.unit == specification.unit &&
      current.persistent == specification.persistent;
    if (!unchanged) {
      return Status::failure(
        "状态字段不能以不同定义重复注册: " + specification.name);
    }
    return Status::success("状态字段已经注册");
  }
  const auto saved = durable_.find(specification.name);
  if (saved != durable_.end()) {
    const auto & previous = saved->second.specification;
    if (previous.owner != specification.owner || previous.type != specification.type ||
      previous.unit != specification.unit || previous.persistent != specification.persistent)
    {return Status::failure("persistent state definition/owner differs: " + specification.name);}
    values_[specification.name] = saved->second.value;
  }
  specifications_.emplace(specification.name, specification);
  return Status::success();
}

Status StateStore::unregister_field(
  const std::string & field_name,
  const std::string & source)
{
  if (!valid_field_name(field_name)) {
    return Status::failure("状态字段名称不合法: " + field_name);
  }
  if (source.empty()) {
    return Status::failure("注销状态字段必须声明来源: " + field_name);
  }

  std::unique_lock<std::shared_mutex> lock(mutex_);
  const auto iterator = specifications_.find(field_name);
  if (iterator == specifications_.end()) {
    return Status::failure("状态字段不存在: " + field_name);
  }
  if (iterator->second.owner != source) {
    return Status::failure("状态字段只能由其所有者注销: " + field_name);
  }
  specifications_.erase(iterator);
  values_.erase(field_name);
  return Status::success("状态字段已注销: " + field_name);
}

Status StateStore::set(
  const std::string & field_name,
  const std::string & value_json,
  const std::string & source,
  const bool persistent)
{
  if (!valid_field_name(field_name)) {
    return Status::failure("状态字段名称不合法: " + field_name);
  }
  if (source.empty()) {
    return Status::failure("状态写入必须提供来源: " + field_name);
  }
  if (!valid_json_value(value_json)) {
    return Status::failure("状态值不是有效的 JSON 基本形式: " + field_name);
  }

  StateValue state_value;
  {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto specification = specifications_.find(field_name);
    StateFieldSpec candidate;
    const bool extension = specification == specifications_.end();
    if (extension) {
      if (!allow_extension_fields_) {return Status::failure("状态字段尚未注册: " + field_name);}
      const auto saved = durable_.find(field_name);
      candidate = saved != durable_.end() ? saved->second.specification :
        StateFieldSpec{field_name, "any", "", source, persistent};
    } else {candidate = specification->second;}
    if (candidate.owner != source) {return Status::failure("状态字段写入来源与所有者不一致: " + field_name);}
    if (!valid_value_for_type(value_json, candidate.type)) {
      return Status::failure("状态值与声明类型不匹配: " + field_name + "，类型=" + candidate.type);
    }
    state_value.value_json = nlohmann::json::parse(value_json).dump();
    state_value.source = source;
    state_value.updated_at = clock_();
    const auto old_value = values_.find(field_name);
    const auto old_durable = durable_.find(field_name);
    state_value.persistent = persistent || candidate.persistent ||
      old_durable != durable_.end();
    if (state_value.persistent) {
      if (!storage_) {return Status::failure("no persistent state backend configured");}
      auto saved = durable_;
      saved[field_name] = {candidate, state_value};
      std::vector<uran_plugin_api::StoredState> records;
      for (const auto & item : saved) {records.push_back(item.second);}
      Status stored;
      try {stored = storage_->save(records);}
      catch (const std::exception & exception) {stored = Status::failure(exception.what());}
      catch (...) {stored = Status::failure("state persistence failed");}
      if (!stored.ok) {return stored;}
      durable_ = std::move(saved);
    }
    const bool changed = old_value == values_.end() ||
      old_value->second.value_json != state_value.value_json ||
      old_value->second.persistent != state_value.persistent;
    if (extension) {specifications_.emplace(field_name, candidate);}
    values_[field_name] = state_value;
    if (!changed) {return Status::success();}
  }

  // 回调在锁外执行，防止订阅者在回调中再次访问状态空间时发生死锁
  observations_->emit(field_name, state_value);
  return Status::success();
}

bool StateStore::contains(const std::string & field_name) const
{
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return values_.find(field_name) != values_.end();
}

bool StateStore::get(const std::string & field_name, StateValue & value) const
{
  std::shared_lock<std::shared_mutex> lock(mutex_);
  const auto iterator = values_.find(field_name);
  if (iterator == values_.end()) {
    return false;
  }
  value = iterator->second;
  return true;
}

std::map<std::string, StateValue> StateStore::snapshot() const
{
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return values_;
}

std::string StateStore::snapshot_json() const
{
  const auto values = snapshot();
  nlohmann::json result = nlohmann::json::object();
  for (const auto & item : values) {
    result[item.first] = nlohmann::json::parse(item.second.value_json);
  }
  return result.dump();
}

std::size_t StateStore::subscribe(ChangeCallback callback)
{
  if (!callback) {
    return 0U;
  }
  return observations_->add(std::move(callback));
}

void StateStore::unsubscribe(const std::size_t subscription_id)
{
  observations_->cancel(subscription_id);
}

std::shared_ptr<uran_plugin_api::StateObservation> StateStore::observe(
  const std::string & field_prefix, uran_plugin_api::StateChangeCallback callback)
{
  if (!callback) {return {};}
  const auto id = subscribe([field_prefix, callback = std::move(callback)](
    const auto & field, const auto & value) {
      if (field_prefix.empty() || field.rfind(field_prefix, 0) == 0) {callback(field, value);}
    });
  // StateStore owns observations only while Runtime/session exists. PluginManager
  // destroys every scoped observation before the session destroys this store.
  return std::make_shared<StoreObservation>(observations_, id);
}

bool StateStore::valid_field_name(const std::string & field_name)
{
  static const std::regex pattern(
    R"(^[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z][A-Za-z0-9_]*)*$)");
  return std::regex_match(field_name, pattern);
}

bool StateStore::supported_type(const std::string & declared_type)
{
  return declared_type.empty() || declared_type == "any" || declared_type == "json" ||
         declared_type == "bool" || declared_type == "string" ||
         declared_type == "int" || declared_type == "int64" ||
         declared_type == "uint" || declared_type == "uint64" ||
         declared_type == "float" || declared_type == "double" ||
         declared_type == "object" || declared_type == "array";
}

bool StateStore::valid_json_value(const std::string & value_json)
{
  const auto value = trim_copy(value_json);
  if (value.empty()) {
    return false;
  }

  return nlohmann::json::accept(value);
}

bool StateStore::valid_value_for_type(
  const std::string & value_json,
  const std::string & declared_type)
{
  if (declared_type.empty() || declared_type == "json" || declared_type == "any") {
    return true;
  }

  const auto value = nlohmann::json::parse(value_json);
  if (declared_type == "bool") {
    return value.is_boolean();
  }
  if (declared_type == "string") {
    return value.is_string();
  }
  if (declared_type == "int" || declared_type == "int64") {
    if (value.is_number_unsigned()) {
      return value.get<std::uint64_t>() <=
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    }
    return value.is_number_integer();
  }
  if (declared_type == "uint" || declared_type == "uint64") {
    return value.is_number_unsigned() ||
           (value.is_number_integer() && value.get<std::int64_t>() >= 0);
  }
  if (declared_type == "float" || declared_type == "double") {
    return value.is_number();
  }
  if (declared_type == "object") {
    return value.is_object();
  }
  if (declared_type == "array") {
    return value.is_array();
  }
  return false;
}

}
