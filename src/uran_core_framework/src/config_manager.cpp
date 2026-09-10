#include "uran_core_framework/config_manager.hpp"

#include <exception>
#include <utility>

namespace uran_core_framework
{

Status ConfigManager::load_file(const std::string & path)
{
  if (path.empty()) {
    return Status::failure("配置文件路径不能为空");
  }

  try {
    YAML::Node root = YAML::LoadFile(path);
    if (!root || !root.IsMap()) {
      return Status::failure("配置文件根节点必须是映射类型: " + path);
    }
    YAML::Node candidate = YAML::Clone(root);

    std::lock_guard<std::mutex> lock(mutex_);
    root_ = std::move(candidate);
    source_path_ = path;
    ++version_;
  } catch (const YAML::Exception & exception) {
    return Status::failure(
      "读取配置文件失败: " + path + ", 原因: " + exception.what());
  }

  return Status::success("配置已加载: " + path);
}

Status ConfigManager::load_node(const YAML::Node & root)
{
  if (!root || !root.IsMap()) {
    return Status::failure("配置根节点必须是映射类型");
  }

  YAML::Node candidate;
  try {
    candidate = YAML::Clone(root);
  } catch (const YAML::Exception & exception) {
    return Status::failure(std::string("复制配置失败: ") + exception.what());
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    root_ = std::move(candidate);
    source_path_.clear();
    ++version_;
  }
  return Status::success("内存配置已加载");
}

YAML::Node ConfigManager::snapshot() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return root_ ? YAML::Clone(root_) : YAML::Node(YAML::NodeType::Map);
}

std::string ConfigManager::source_path() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return source_path_;
}

std::uint64_t ConfigManager::version() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return version_;
}

}
