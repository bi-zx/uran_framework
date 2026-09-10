#pragma once

#include <map>
#include <memory>
#include <string>

#include "uran_plugin_api/runtime_access.hpp"

#include "uran_core_framework/visibility_control.hpp"

namespace uran_core_framework
{

/**
 * @brief 将状态写权限绑定到单一插件所有者。
 *
 * 插件只能以自己的 ID 注册和写入状态字段，不能通过伪造 source 修改其他模块的状态。
 * 读取接口仍委托给底层状态空间，后续可以在该边界继续增加字段级读取权限。
 */
class URAN_CORE_FRAMEWORK_PUBLIC ScopedStateAccess final
  : public uran_plugin_api::StateAccess
{
public:
  ScopedStateAccess(
    std::shared_ptr<uran_plugin_api::StateAccess> upstream,
    std::string owner);

  uran_plugin_api::Status register_field(
    const uran_plugin_api::StateFieldSpec & specification) override;
  uran_plugin_api::Status set(
    const std::string & field_name,
    const std::string & value_json,
    const std::string & source,
    bool persistent = false) override;
  bool get(
    const std::string & field_name,
    uran_plugin_api::StateValue & value) const override;
  std::map<std::string, uran_plugin_api::StateValue> snapshot() const override;
  std::string snapshot_json() const override;

  const std::string & owner() const noexcept;

private:
  std::shared_ptr<uran_plugin_api::StateAccess> upstream_;
  std::string owner_;
};

}
