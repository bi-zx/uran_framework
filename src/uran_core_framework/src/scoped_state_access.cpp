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
  auto owned_specification = specification;
  if (owned_specification.owner.empty()) {
    owned_specification.owner = owner_;
  }
  if (owned_specification.owner != owner_) {
    return uran_plugin_api::Status::failure(
      "插件不能注册其他所有者的状态字段: " + specification.name);
  }
  return upstream_->register_field(owned_specification);
}

uran_plugin_api::Status ScopedStateAccess::unregister_field(
  const std::string & field_name,
  const std::string & source)
{
  if (source != owner_) {
    return uran_plugin_api::Status::failure(
      "插件只能注销自身运行实例拥有的状态字段: " + field_name);
  }
  return upstream_->unregister_field(field_name, owner_);
}

uran_plugin_api::Status ScopedStateAccess::set(
  const std::string & field_name,
  const std::string & value_json,
  const std::string & source,
  const bool persistent)
{
  if (source != owner_) {
    return uran_plugin_api::Status::failure(
      "插件状态写入来源必须与运行实例标识一致: " + field_name);
  }
  return upstream_->set(field_name, value_json, owner_, persistent);
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

}
