#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include <yaml-cpp/yaml.h>

#include "uran_core_framework/types.hpp"
#include "uran_core_framework/visibility_control.hpp"

namespace uran_core_framework
{

/** @brief 以线程安全快照方式保存 Runtime 配置。 */
class URAN_CORE_FRAMEWORK_PUBLIC ConfigManager final
{
public:
  Status load_file(const std::string & path);
  Status load_node(const YAML::Node & root);

  YAML::Node snapshot() const;
  std::string source_path() const;
  std::uint64_t version() const;

private:
  mutable std::mutex mutex_;
  YAML::Node root_;
  std::string source_path_;
  std::uint64_t version_{0};
};

}