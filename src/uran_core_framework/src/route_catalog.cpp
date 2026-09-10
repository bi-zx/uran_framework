#include "uran_core_framework/route_catalog.hpp"

#include <utility>

namespace uran_core_framework
{

RouteCatalog::RouteCatalog(std::map<std::string, std::string> routes)
: routes_(std::move(routes))
{
}

Status RouteCatalog::load_file(const std::string & path)
{
  if (path.empty()) {
    return Status::failure("路由配置文件路径不能为空");
  }

  try {
    return load_node(YAML::LoadFile(path));
  } catch (const YAML::Exception & exception) {
    return Status::failure(
      "读取路由配置失败: " + path + ", 原因: " + exception.what());
  }
}

Status RouteCatalog::load_node(const YAML::Node & root)
{
  if (!root || !root.IsMap()) {
    return Status::failure("路由配置根节点必须是映射类型");
  }
  const YAML::Node routes = root["routes"] ? root["routes"] : root;
  if (!routes || !routes.IsMap()) {
    return Status::failure("路由配置根节点必须是映射类型");
  }

  std::map<std::string, std::string> parsed;
  try {
    for (const auto & item : routes) {
      const auto name = item.first.as<std::string>();
      if (!item.second.IsScalar()) {
        return Status::failure("路由值必须是字符串: " + name);
      }
      const auto route = item.second.as<std::string>();
      if (name.empty() || route.empty()) {
        return Status::failure("路由名称和值不能为空");
      }
      parsed.emplace(name, route);
    }
  } catch (const YAML::Exception & exception) {
    return Status::failure(std::string("解析路由配置失败: ") + exception.what());
  }

  std::lock_guard<std::mutex> lock(mutex_);
  routes_ = std::move(parsed);
  return Status::success("路由配置已加载");
}

Status RouteCatalog::set(const std::string & route_name, const std::string & route)
{
  if (route_name.empty() || route.empty()) {
    return Status::failure("路由名称和值不能为空");
  }

  std::lock_guard<std::mutex> lock(mutex_);
  routes_[route_name] = route;
  return Status::success();
}

bool RouteCatalog::resolve(const std::string & route_name, std::string & route) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  const auto iterator = routes_.find(route_name);
  if (iterator == routes_.end()) {
    return false;
  }
  route = iterator->second;
  return true;
}

std::map<std::string, std::string> RouteCatalog::snapshot() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return routes_;
}

}
