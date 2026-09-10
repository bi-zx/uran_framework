#include "uran_core_framework/plugin_manager.hpp"

#include <algorithm>
#include <exception>
#include <regex>
#include <stdexcept>
#include <utility>

#include <pluginlib/exceptions.hpp>

#include "uran_core_framework/scoped_state_access.hpp"

namespace uran_core_framework
{

namespace
{

Status parse_plugin_spec(const YAML::Node & node, PluginSpec & specification)
{
  if (!node || !node.IsMap()) {
    return Status::failure("插件配置项必须是映射类型");
  }

  try {
    specification.descriptor.plugin_id = node["id"].as<std::string>();
    specification.descriptor.class_type = node["class_type"].as<std::string>();
    specification.descriptor.plugin_type =
      node["plugin_type"] ? node["plugin_type"].as<std::string>() : std::string{};
    specification.descriptor.plugin_version =
      node["plugin_version"] ? node["plugin_version"].as<std::string>() : std::string{};
    specification.descriptor.api_version =
      node["api_version"] ? node["api_version"].as<std::string>() : std::string{};
    specification.enabled = node["enabled"] ? node["enabled"].as<bool>() : true;
    specification.priority = node["priority"] ? node["priority"].as<int>() : 0;
    specification.config_file =
      node["config_file"] ? node["config_file"].as<std::string>() : std::string{};
  } catch (const YAML::Exception & exception) {
    return Status::failure(std::string("插件配置字段无效: ") + exception.what());
  }

  return Status::success();
}

}

PluginManager::PluginManager(std::string base_package)
: base_package_(std::move(base_package)),
  loader_(base_package_, "uran_plugin_api::Plugin")
{
  if (base_package_.empty()) {
    throw std::invalid_argument("插件基类包名不能为空");
  }
}

PluginManager::~PluginManager()
{
  std::vector<std::string> plugin_ids;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    plugin_ids.reserve(entries_.size());
    for (const auto & item : entries_) {
      plugin_ids.push_back(item.first);
    }
  }

  // 析构阶段采用尽力清理策略，禁止异常穿过析构函数边界。
  for (const auto & plugin_id : plugin_ids) {
    try {
      static_cast<void>(unload(plugin_id));
    } catch (...) {
    }
  }
}

Status PluginManager::load(
  const PluginSpec & specification,
  const uran_plugin_api::PluginContext & context,
  const YAML::Node & config)
{
  const auto & requested = specification.descriptor;
  if (!valid_plugin_id(requested.plugin_id)) {
    return Status::failure("插件 ID 不合法: " + requested.plugin_id);
  }
  if (requested.class_type.empty()) {
    return Status::failure("插件 class_type 不能为空: " + requested.plugin_id);
  }
  if (!specification.enabled) {
    return Status::failure("插件已在配置中禁用: " + requested.plugin_id);
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (entries_.find(requested.plugin_id) != entries_.end()) {
      return Status::failure("插件 ID 已存在: " + requested.plugin_id);
    }
    Entry placeholder;
    placeholder.specification = specification;
    placeholder.transitioning = true;
    entries_.emplace(requested.plugin_id, std::move(placeholder));
  }

  // 保证局部变量析构时先销毁插件实例，再销毁它使用的上下文
  std::shared_ptr<uran_plugin_api::PluginContext> plugin_context;
  std::shared_ptr<uran_plugin_api::Plugin> instance;
  try {
    // ClassLoader 的内部状态不作为并发契约暴露，显式串行化类加载操作
    std::lock_guard<std::mutex> loader_lock(loader_mutex_);
    instance = loader_.createSharedInstance(requested.class_type);
  } catch (const pluginlib::PluginlibException & exception) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(requested.plugin_id);
    return Status::failure(
      "加载插件类失败: " + requested.class_type + ", 原因: " + exception.what());
  } catch (const std::exception & exception) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(requested.plugin_id);
    return Status::failure(
      "创建插件实例失败: " + requested.class_type + ", 原因: " + exception.what());
  } catch (...) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(requested.plugin_id);
    return Status::failure("创建插件实例时发生未知异常: " + requested.class_type);
  }

  if (!instance) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(requested.plugin_id);
    return Status::failure("插件加载器返回空实例: " + requested.class_type);
  }

  PluginDescriptor actual;
  try {
    actual = instance->descriptor();
  } catch (const std::exception & exception) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(requested.plugin_id);
    return Status::failure(
      "读取插件描述失败: " + requested.plugin_id + ", 原因: " + exception.what());
  } catch (...) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(requested.plugin_id);
    return Status::failure("读取插件描述时发生未知异常: " + requested.plugin_id);
  }
  if (actual.plugin_id != requested.plugin_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(requested.plugin_id);
    return Status::failure(
      "插件描述 ID 与配置不一致，配置=" + requested.plugin_id +
      "，实现=" + actual.plugin_id);
  }
  if (actual.plugin_type.empty() || actual.api_version.empty() || actual.class_type.empty()) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(requested.plugin_id);
    return Status::failure("插件描述缺少类型、API 版本或实现类: " + requested.plugin_id);
  }
  if (actual.class_type != requested.class_type) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(requested.plugin_id);
    return Status::failure("插件描述实现类与配置不一致: " + requested.plugin_id);
  }
  if (!requested.plugin_type.empty() && actual.plugin_type != requested.plugin_type) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(requested.plugin_id);
    return Status::failure("插件类型与配置不一致: " + requested.plugin_id);
  }
  if (!requested.api_version.empty() && actual.api_version != requested.api_version) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(requested.plugin_id);
    return Status::failure("插件 API 版本不兼容: " + requested.plugin_id);
  }
  if (!requested.plugin_version.empty() &&
    actual.plugin_version != requested.plugin_version)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(requested.plugin_id);
    return Status::failure("插件实现版本与配置不一致: " + requested.plugin_id);
  }

  try {
    const auto node = context.node();
    if (!node) {
      std::lock_guard<std::mutex> lock(mutex_);
      entries_.erase(requested.plugin_id);
      return Status::failure("配置插件时 Runtime 节点已经失效: " + requested.plugin_id);
    }
    auto state_access = std::make_shared<ScopedStateAccess>(
      context.state_access(), requested.plugin_id);
    plugin_context = std::make_shared<uran_plugin_api::PluginContext>(
      node, std::move(state_access), context.route_resolver(),
      context.instance_namespace());
  } catch (const std::exception & exception) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(requested.plugin_id);
    return Status::failure(
      "创建插件受控上下文失败: " + requested.plugin_id +
      "，原因: " + exception.what());
  }

  Status configured;
  try {
    configured = instance->configure(*plugin_context, config);
  } catch (const std::exception & exception) {
    configured = Status::failure(std::string("插件 configure 抛出异常: ") + exception.what());
  } catch (...) {
    configured = Status::failure("插件 configure 抛出未知异常");
  }

  if (!configured.ok) {
    try {
      static_cast<void>(instance->cleanup());
    } catch (...) {
    }
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(requested.plugin_id);
    return Status::failure(
      "配置插件失败: " + requested.plugin_id + ", 原因: " + configured.message);
  }

  bool cancelled = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto iterator = entries_.find(requested.plugin_id);
    if (iterator == entries_.end()) {
      cancelled = true;
    } else {
      iterator->second.context = std::move(plugin_context);
      iterator->second.instance = instance;
      iterator->second.state = PluginState::configured;
      iterator->second.transitioning = false;
      iterator->second.error.clear();
    }
  }
  if (cancelled) {
    // 插件方法始终在管理器锁外调用，避免插件回调 Runtime 时形成锁重入
    try {
      static_cast<void>(instance->cleanup());
    } catch (...) {
    }
    return Status::failure("插件加载事务已被取消: " + requested.plugin_id);
  }
  return Status::success("插件已配置: " + requested.plugin_id);
}

Status PluginManager::activate(const std::string & plugin_id)
{
  std::shared_ptr<uran_plugin_api::Plugin> instance;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto * entry = find_entry(plugin_id);
    if (entry == nullptr) {
      return Status::failure("插件不存在: " + plugin_id);
    }
    if (entry->transitioning) {
      return Status::failure("插件正在执行其他生命周期操作: " + plugin_id);
    }
    if (entry->state != PluginState::configured && entry->state != PluginState::inactive) {
      return Status::failure("当前状态不允许激活插件: " + plugin_id);
    }
    entry->transitioning = true;
    instance = entry->instance;
  }

  Status result;
  try {
    result = instance->activate();
  } catch (const std::exception & exception) {
    result = Status::failure(std::string("插件 activate 抛出异常: ") + exception.what());
  } catch (...) {
    result = Status::failure("插件 activate 抛出未知异常");
  }

  std::lock_guard<std::mutex> lock(mutex_);
  auto * entry = find_entry(plugin_id);
  if (entry == nullptr || entry->instance != instance) {
    return Status::failure("插件激活事务失效: " + plugin_id);
  }
  entry->transitioning = false;
  entry->state = result.ok ? PluginState::active : PluginState::error;
  entry->error = result.ok ? std::string{} : result.message;
  return result;
}

Status PluginManager::deactivate(const std::string & plugin_id)
{
  std::shared_ptr<uran_plugin_api::Plugin> instance;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto * entry = find_entry(plugin_id);
    if (entry == nullptr) {
      return Status::failure("插件不存在: " + plugin_id);
    }
    if (entry->transitioning) {
      return Status::failure("插件正在执行其他生命周期操作: " + plugin_id);
    }
    if (entry->state == PluginState::inactive || entry->state == PluginState::configured) {
      return Status::success("插件已经停止: " + plugin_id);
    }
    if (entry->state != PluginState::active && entry->state != PluginState::error) {
      return Status::failure("当前状态不允许停用插件: " + plugin_id);
    }
    entry->transitioning = true;
    instance = entry->instance;
  }

  Status result;
  try {
    result = instance->deactivate();
  } catch (const std::exception & exception) {
    result = Status::failure(std::string("插件 deactivate 抛出异常: ") + exception.what());
  } catch (...) {
    result = Status::failure("插件 deactivate 抛出未知异常");
  }

  std::lock_guard<std::mutex> lock(mutex_);
  auto * entry = find_entry(plugin_id);
  if (entry == nullptr || entry->instance != instance) {
    return Status::failure("插件停用事务失效: " + plugin_id);
  }
  entry->transitioning = false;
  entry->state = result.ok ? PluginState::inactive : PluginState::error;
  entry->error = result.ok ? std::string{} : result.message;
  return result;
}

Status PluginManager::unload(const std::string & plugin_id)
{
  PluginState state;
  // 上下文局部副本必须比插件实例局部副本存活久
  std::shared_ptr<uran_plugin_api::PluginContext> plugin_context;
  std::shared_ptr<uran_plugin_api::Plugin> instance;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto * entry = find_entry(plugin_id);
    if (entry == nullptr) {
      return Status::failure("插件不存在: " + plugin_id);
    }
    if (entry->transitioning) {
      return Status::failure("插件正在执行其他生命周期操作: " + plugin_id);
    }
    if (!entry->instance) {
      return Status::failure("插件实例尚未完成创建: " + plugin_id);
    }
    state = entry->state;
    plugin_context = entry->context;
    instance = entry->instance;
    // 停用和清理，占用一个生命周期事务
    entry->transitioning = true;
  }
  static_cast<void>(plugin_context);

  if (state == PluginState::active || state == PluginState::error) {
    Status stopped;
    try {
      stopped = instance->deactivate();
    } catch (const std::exception & exception) {
      stopped = Status::failure(std::string("插件 deactivate 抛出异常: ") + exception.what());
    } catch (...) {
      stopped = Status::failure("插件 deactivate 抛出未知异常");
    }
    if (!stopped.ok) {
      std::lock_guard<std::mutex> lock(mutex_);
      auto * entry = find_entry(plugin_id);
      if (entry != nullptr && entry->instance == instance) {
        entry->transitioning = false;
        entry->state = PluginState::error;
        entry->error = stopped.message;
      }
      return stopped;
    }
  }

  Status cleaned;
  try {
    cleaned = instance->cleanup();
  } catch (const std::exception & exception) {
    cleaned = Status::failure(std::string("插件 cleanup 抛出异常: ") + exception.what());
  } catch (...) {
    cleaned = Status::failure("插件 cleanup 抛出未知异常");
  }

  std::lock_guard<std::mutex> lock(mutex_);
  auto * entry = find_entry(plugin_id);
  if (entry == nullptr || entry->instance != instance) {
    return Status::failure("插件清理事务失效: " + plugin_id);
  }
  if (!cleaned.ok) {
    entry->transitioning = false;
    entry->state = PluginState::error;
    entry->error = cleaned.message;
    return cleaned;
  }
  entries_.erase(plugin_id);
  return Status::success("插件已卸载: " + plugin_id);
}

Status PluginManager::configure_and_load_all(
  const YAML::Node & plugins_node,
  const uran_plugin_api::PluginContext & context)
{
  YAML::Node list = plugins_node;
  if (plugins_node && plugins_node.IsMap() && plugins_node["plugins"]) {
    list = plugins_node["plugins"];
  }
  if (!list || !list.IsSequence()) {
    return Status::failure("插件配置必须是序列类型");
  }

  struct Pending
  {
    PluginSpec specification;
    YAML::Node config;
    bool auto_activate{true};
  };
  std::vector<Pending> pending;

  for (const auto & item : list) {
    Pending value;
    const auto parsed = parse_plugin_spec(item, value.specification);
    if (!parsed.ok) {
      return parsed;
    }
    if (!value.specification.enabled) {
      continue;
    }
    try {
      value.auto_activate = item["auto_activate"] ? item["auto_activate"].as<bool>() : true;
      if (!value.specification.config_file.empty()) {
        value.config = YAML::LoadFile(value.specification.config_file);
      } else {
        value.config = item["config"] ? YAML::Clone(item["config"]) :
          YAML::Node(YAML::NodeType::Map);
      }
    } catch (const YAML::Exception & exception) {
      return Status::failure(
        "读取插件私有配置失败: " + value.specification.descriptor.plugin_id +
        ", 原因: " + exception.what());
    }
    pending.push_back(std::move(value));
  }

  std::stable_sort(
    pending.begin(), pending.end(),
    [](const Pending & left, const Pending & right) {
      return left.specification.priority > right.specification.priority;
    });

  std::vector<std::string> loaded_ids;
  for (const auto & item : pending) {
    const auto loaded = load(item.specification, context, item.config);
    if (!loaded.ok) {
      for (auto iterator = loaded_ids.rbegin(); iterator != loaded_ids.rend(); ++iterator) {
        static_cast<void>(unload(*iterator));
      }
      return loaded;
    }
    loaded_ids.push_back(item.specification.descriptor.plugin_id);

    if (item.auto_activate) {
      const auto activated = activate(item.specification.descriptor.plugin_id);
      if (!activated.ok) {
        for (auto iterator = loaded_ids.rbegin(); iterator != loaded_ids.rend(); ++iterator) {
          static_cast<void>(unload(*iterator));
        }
        return activated;
      }
    }
  }
  return Status::success("插件配置加载完成");
}

std::vector<PluginRuntimeInfo> PluginManager::list() const
{
  struct SnapshotEntry
  {
    PluginSpec specification;
    std::shared_ptr<uran_plugin_api::Plugin> instance;
    PluginState state{PluginState::unloaded};
    std::string error;
  };
  std::vector<SnapshotEntry> snapshot;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot.reserve(entries_.size());
    for (const auto & item : entries_) {
      snapshot.push_back(SnapshotEntry{
        item.second.specification,
        item.second.instance,
        item.second.state,
        item.second.error});
    }
  }

  std::vector<PluginRuntimeInfo> result;
  result.reserve(snapshot.size());
  for (const auto & item : snapshot) {
    PluginRuntimeInfo info;
    info.descriptor = item.specification.descriptor;
    info.state = item.state;
    info.error = item.error;
    if (item.instance) {
      try {
        info.descriptor = item.instance->descriptor();
        info.health = item.instance->health();
        info.capabilities = item.instance->capabilities();
      } catch (const std::exception & exception) {
        info.health.level = PluginHealth::Level::error;
        info.health.message = exception.what();
      } catch (...) {
        info.health.level = PluginHealth::Level::error;
        info.health.message = "读取插件运行信息时发生未知异常";
      }
    }
    result.push_back(std::move(info));
  }
  return result;
}

std::vector<std::shared_ptr<uran_plugin_api::Plugin>> PluginManager::instances() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::shared_ptr<uran_plugin_api::Plugin>> result;
  result.reserve(entries_.size());
  for (const auto & item : entries_) {
    if (item.second.instance) {
      result.push_back(item.second.instance);
    }
  }
  return result;
}

std::vector<std::shared_ptr<uran_plugin_api::Plugin>> PluginManager::active_instances() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::shared_ptr<uran_plugin_api::Plugin>> result;
  result.reserve(entries_.size());
  for (const auto & item : entries_) {
    if (!item.second.transitioning &&
      item.second.state == PluginState::active && item.second.instance)
    {
      result.push_back(item.second.instance);
    }
  }
  return result;
}

bool PluginManager::has(const std::string & plugin_id) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.find(plugin_id) != entries_.end();
}

PluginManager::Entry * PluginManager::find_entry(const std::string & plugin_id)
{
  const auto iterator = entries_.find(plugin_id);
  return iterator == entries_.end() ? nullptr : &iterator->second;
}

const PluginManager::Entry * PluginManager::find_entry(const std::string & plugin_id) const
{
  const auto iterator = entries_.find(plugin_id);
  return iterator == entries_.end() ? nullptr : &iterator->second;
}

bool PluginManager::valid_plugin_id(const std::string & plugin_id)
{
  static const std::regex pattern(R"(^[A-Za-z][A-Za-z0-9_.-]*$)");
  return std::regex_match(plugin_id, pattern);
}

}