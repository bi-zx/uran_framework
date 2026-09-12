#include "uran_core_framework/route_catalog.hpp"

#include <utility>
#include <regex>
#include <stdexcept>

namespace uran_core_framework
{
namespace {
bool semantic_name(const std::string & name)
{
  static const std::regex pattern(R"(^[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z][A-Za-z0-9_]*)*$)");
  return std::regex_match(name, pattern);
}
}

RouteCatalog::RouteCatalog(std::map<std::string, std::string> routes)
: routes_(std::move(routes))
{
  for (const auto & item : routes_) {
    if (!semantic_name(item.first) || !semantic_name(item.second)) {
      throw std::invalid_argument("route aliases must name semantic messages");
    }
  }
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
      if (!semantic_name(name) || !semantic_name(route)) {
        return Status::failure("路由名称和值必须是点分语义标识");
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
  if (!semantic_name(route_name) || !semantic_name(route)) {
    return Status::failure("路由名称和值必须是点分语义标识");
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
