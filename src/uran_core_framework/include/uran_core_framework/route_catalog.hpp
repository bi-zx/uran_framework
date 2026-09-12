#pragma once

#include <map>
#include <mutex>
#include <string>

#include <yaml-cpp/yaml.h>

#include "uran_core_framework/types.hpp"
#include "uran_core_framework/visibility_control.hpp"
#include "uran_plugin_api/runtime_access.hpp"

namespace uran_core_framework
{

/** @brief 保存逻辑路由别名到消息语义标识的线程安全映射；平台端点由适配器管理。 */
class URAN_CORE_FRAMEWORK_PUBLIC RouteCatalog final
  : public uran_plugin_api::RouteResolver
{
public:
  RouteCatalog() = default;
  explicit RouteCatalog(std::map<std::string, std::string> routes);

  Status load_file(const std::string & path);
  Status load_node(const YAML::Node & root);

  Status set(const std::string & route_name, const std::string & route);
  bool resolve(const std::string & route_name, std::string & route) const override;
  std::map<std::string, std::string> snapshot() const;

private:
  mutable std::mutex mutex_;
  std::map<std::string, std::string> routes_;
};

}
