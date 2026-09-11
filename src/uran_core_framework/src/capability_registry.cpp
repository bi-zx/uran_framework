#include "uran_core_framework/capability_registry.hpp"

#include <algorithm>
#include <regex>
#include <set>

namespace uran_core_framework
{

Status CapabilityRegistry::register_instance(
  const PluginIdentity & identity,
  const std::vector<std::string> & capability_ids,
  const int priority)
{
  if (!valid_identifier(identity.instance_id) ||
    !valid_identifier(identity.device_id) ||
    !valid_identifier(identity.component_id))
  {
    return Status::failure("能力提供者的实例、设备或组件标识不合法");
  }

  std::set<std::string> unique_capabilities;
  for (const auto & capability_id : capability_ids) {
    if (!valid_capability_id(capability_id)) {
      return Status::failure("能力标识不合法: " + capability_id);
    }
    if (!unique_capabilities.insert(capability_id).second) {
      return Status::failure("实例重复声明能力: " + capability_id);
    }
  }

  std::lock_guard<std::mutex> lock(mutex_);
  const auto identity_iterator = identities_.find(identity.instance_id);
  if (identity_iterator != identities_.end() && identity_iterator->second != identity) {
    return Status::failure(
      "同一实例标识不能归属于不同设备或组件: " + identity.instance_id);
  }
  for (const auto & capability_id : unique_capabilities) {
    const auto capability_iterator = providers_.find(capability_id);
    if (capability_iterator != providers_.end() &&
      capability_iterator->second.find(identity.instance_id) !=
      capability_iterator->second.end())
    {
      return Status::failure(
        "能力提供者已经注册: " + capability_id + "/" + identity.instance_id);
    }
  }

  identities_[identity.instance_id] = identity;
  for (const auto & capability_id : unique_capabilities) {
    providers_[capability_id].emplace(
      identity.instance_id,
      CapabilityProvider{capability_id, identity, priority});
  }
  return Status::success("实例能力已注册: " + identity.instance_id);
}

Status CapabilityRegistry::unregister_instance(const std::string & instance_id)
{
  if (!valid_identifier(instance_id)) {
    return Status::failure("插件实例标识不合法: " + instance_id);
  }

  std::lock_guard<std::mutex> lock(mutex_);
  identities_.erase(instance_id);
  for (auto iterator = providers_.begin(); iterator != providers_.end();) {
    iterator->second.erase(instance_id);
    if (iterator->second.empty()) {
      iterator = providers_.erase(iterator);
    } else {
      ++iterator;
    }
  }
  return Status::success("实例能力已撤销: " + instance_id);
}

std::vector<CapabilityProvider> CapabilityRegistry::providers(
  const std::string & capability_id,
  const std::string & device_id,
  const std::string & component_id) const
{
  std::vector<CapabilityProvider> result;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto iterator = providers_.find(capability_id);
    if (iterator == providers_.end()) {
      return result;
    }
    for (const auto & item : iterator->second) {
      const auto & provider = item.second;
      if (!device_id.empty() && provider.identity.device_id != device_id) {
        continue;
      }
      if (!component_id.empty() && provider.identity.component_id != component_id) {
        continue;
      }
      result.push_back(provider);
    }
  }

  std::stable_sort(
    result.begin(), result.end(),
    [](const CapabilityProvider & left, const CapabilityProvider & right) {
      if (left.priority != right.priority) {
        return left.priority > right.priority;
      }
      return left.identity.instance_id < right.identity.instance_id;
    });
  return result;
}

std::vector<std::string> CapabilityRegistry::capabilities_for_instance(
  const std::string & instance_id) const
{
  std::vector<std::string> result;
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto & capability : providers_) {
    if (capability.second.find(instance_id) != capability.second.end()) {
      result.push_back(capability.first);
    }
  }
  return result;
}

std::size_t CapabilityRegistry::capability_count() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return providers_.size();
}

std::size_t CapabilityRegistry::provider_count() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  std::size_t count = 0U;
  for (const auto & capability : providers_) {
    count += capability.second.size();
  }
  return count;
}

bool CapabilityRegistry::valid_identifier(const std::string & value)
{
  static const std::regex pattern(R"(^[A-Za-z][A-Za-z0-9_]*$)");
  return std::regex_match(value, pattern);
}

bool CapabilityRegistry::valid_capability_id(const std::string & value)
{
  static const std::regex pattern(
    R"(^[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z][A-Za-z0-9_]*)*$)");
  return std::regex_match(value, pattern);
}

}
