#include "uran_core_framework/scoped_state_access.hpp"

#include <stdexcept>
#include <utility>

namespace uran_core_framework
{

ScopedStateAccess::ScopedStateAccess(
  std::shared_ptr<uran_plugin_api::StateAccess> upstream,
  std::string owner)
: upstream_(std::move(upstream)), owner_(std::move(owner))
{
  if (!upstream_) {
    throw std::invalid_argument("ScopedStateAccess 的底层状态接口不能为空");
  }
  if (owner_.empty()) {
    throw std::invalid_argument("ScopedStateAccess 的所有者不能为空");
  }
}

uran_plugin_api::Status ScopedStateAccess::register_field(
  const uran_plugin_api::StateFieldSpec & specification)
{
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (revoked_) {return uran_plugin_api::Status::failure("state access revoked");}
  auto owned_specification = specification;
  if (owned_specification.owner.empty()) {
    owned_specification.owner = owner_;
  }
  if (owned_specification.owner != owner_) {
    return uran_plugin_api::Status::failure(
      "插件不能注册其他所有者的状态字段: " + specification.name);
  }
  const auto result = upstream_->register_field(owned_specification);
  if (result.ok) {fields_.insert(specification.name);}
  return result;
}

uran_plugin_api::Status ScopedStateAccess::unregister_field(
  const std::string & field_name,
  const std::string & source)
{
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (revoked_) {return uran_plugin_api::Status::failure("state access revoked");}
  if (source != owner_) {
    return uran_plugin_api::Status::failure(
      "插件只能注销自身运行实例拥有的状态字段: " + field_name);
  }
  const auto result = upstream_->unregister_field(field_name, owner_);
  if (result.ok) {fields_.erase(field_name);}
  return result;
}

uran_plugin_api::Status ScopedStateAccess::set(
  const std::string & field_name,
  const std::string & value_json,
  const std::string & source,
  const bool persistent)
{
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (revoked_) {return uran_plugin_api::Status::failure("state access revoked");}
  if (source != owner_) {
    return uran_plugin_api::Status::failure(
      "插件状态写入来源必须与运行实例标识一致: " + field_name);
  }
  const auto result = upstream_->set(field_name, value_json, owner_, persistent);
  if (result.ok) {fields_.insert(field_name);}
  return result;
}

bool ScopedStateAccess::get(
  const std::string & field_name,
  uran_plugin_api::StateValue & value) const
{
  return upstream_->get(field_name, value);
}

std::map<std::string, uran_plugin_api::StateValue> ScopedStateAccess::snapshot() const
{
  return upstream_->snapshot();
}

std::string ScopedStateAccess::snapshot_json() const
{
  return upstream_->snapshot_json();
}

const std::string & ScopedStateAccess::owner() const noexcept
{
  return owner_;
}

ScopedStateAccess::~ScopedStateAccess() {revoke();}

void ScopedStateAccess::revoke() noexcept
{
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  revoked_ = true;
  for (const auto & field : fields_) {
    try {static_cast<void>(upstream_->unregister_field(field, owner_));} catch (...) {}
  }
  fields_.clear();
}

}
