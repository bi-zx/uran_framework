#include "uran_core_framework/state_store.hpp"

#include <chrono>
#include <cstdint>
#include <limits>
#include <regex>
#include <utility>

#include <nlohmann/json.hpp>

namespace uran_core_framework
{

namespace
{

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

StateStore::StateStore(const bool allow_extension_fields)
: allow_extension_fields_(allow_extension_fields)
{
}

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
  std::vector<ChangeCallback> callbacks;
  {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto specification = specifications_.find(field_name);
    if (specification == specifications_.end()) {
      if (!allow_extension_fields_) {
        return Status::failure("状态字段尚未注册: " + field_name);
      }
      // 扩展字段首次写入时即绑定所有者，后续不能由其他模块接管
      StateFieldSpec extension_specification;
      extension_specification.name = field_name;
      extension_specification.type = "any";
      extension_specification.owner = source;
      extension_specification.persistent = persistent;
      specification = specifications_.emplace(
        field_name, std::move(extension_specification)).first;
    }
    if (specification->second.owner != source) {
      return Status::failure(
        "状态字段写入来源与所有者不一致: " + field_name);
    }
    if (!valid_value_for_type(value_json, specification->second.type)) {
      return Status::failure(
        "状态值与声明类型不匹配: " + field_name +
        "，类型=" + specification->second.type);
    }

    state_value.value_json = trim_copy(value_json);
    state_value.source = source;
    state_value.updated_at = std::chrono::system_clock::now();
    state_value.persistent = persistent || specification->second.persistent;
    values_[field_name] = state_value;

    callbacks.reserve(callbacks_.size());
    for (const auto & item : callbacks_) {
      callbacks.push_back(item.second);
    }
  }

  // 回调在锁外执行，防止订阅者在回调中再次访问状态空间时发生死锁
  for (const auto & callback : callbacks) {
    if (callback) {
      try {
        callback(field_name, state_value);
      } catch (...) {
        // 单个观察者故障不能改变已经提交的状态，也不能阻断其他观察者
      }
    }
  }
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
  std::unique_lock<std::shared_mutex> lock(mutex_);
  const auto id = next_subscription_id_++;
  callbacks_.emplace(id, std::move(callback));
  return id;
}

void StateStore::unsubscribe(const std::size_t subscription_id)
{
  std::unique_lock<std::shared_mutex> lock(mutex_);
  callbacks_.erase(subscription_id);
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
    if (value.is_number_integer()) {
      return true;
    }
    return value.is_number_unsigned() &&
           value.get<std::uint64_t>() <=
           static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
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
