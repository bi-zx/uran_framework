#include "uran_core_framework/plugin_manager.hpp"

#include <algorithm>
#include <exception>
#include <regex>
#include <stdexcept>
#include <utility>

#include <pluginlib/exceptions.hpp>

#include "uran_core_framework/capability_registry.hpp"
#include "uran_core_framework/scoped_state_access.hpp"

namespace uran_core_framework
{

namespace
{

Status parse_plugin_spec(
  const YAML::Node & node,
  const std::string & default_device_id,
  PluginSpec & specification)
{
  if (!node || !node.IsMap()) {
    return Status::failure("插件配置项必须是映射类型");
  }

  try {
    specification.identity.instance_id = node["instance_id"].as<std::string>();
    specification.identity.device_id = node["device_id"] ?
      node["device_id"].as<std::string>() : default_device_id;
    specification.identity.component_id = node["component_id"].as<std::string>();
    specification.descriptor.class_type = node["class_type"].as<std::string>();
    specification.descriptor.plugin_type =
      node["plugin_type"] ? node["plugin_type"].as<std::string>() : std::string{};
    specification.descriptor.plugin_version =
      node["plugin_version"] ? node["plugin_version"].as<std::string>() : std::string{};
    specification.descriptor.api_version =
      node["api_version"] ? node["api_version"].as<std::string>() : std::string{};
    specification.ros_namespace =
      node["ros_namespace"] ? node["ros_namespace"].as<std::string>() : std::string{};
    specification.enabled = node["enabled"] ? node["enabled"].as<bool>() : true;
    specification.priority = node["priority"] ? node["priority"].as<int>() : 0;
    specification.config_file =
      node["config_file"] ? node["config_file"].as<std::string>() : std::string{};
  } catch (const YAML::Exception & exception) {
    return Status::failure(std::string("插件配置字段无效: ") + exception.what());
  }

  return Status::success();
}

std::string build_instance_namespace(
  std::string base_namespace,
  const PluginIdentity & identity)
{
  while (base_namespace.size() > 1U && base_namespace.back() == '/') {
    base_namespace.pop_back();
  }
  if (base_namespace == "/") {
    base_namespace.clear();
  }
  return base_namespace + "/" + identity.device_id + "/" +
         identity.component_id + "/" + identity.instance_id;
}

}

PluginManager::PluginManager(
  std::string base_package,
  std::shared_ptr<CapabilityRegistry> capability_registry)
: base_package_(std::move(base_package)),
  capability_registry_(std::move(capability_registry)),
  loader_(base_package_, "uran_plugin_api::Plugin")
{
  if (base_package_.empty()) {
    throw std::invalid_argument("插件基类包名不能为空");
  }
}

PluginManager::~PluginManager()
{
  std::vector<std::string> instance_ids;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    instance_ids.reserve(entries_.size());
    for (const auto & item : entries_) {
      instance_ids.push_back(item.first);
    }
  }

  // 析构阶段采用尽力清理策略，禁止异常穿过析构函数边界。
  for (const auto & instance_id : instance_ids) {
    try {
      static_cast<void>(unload(instance_id));
    } catch (...) {
    }
  }
}

Status PluginManager::load(
  const PluginSpec & specification,
  const uran_plugin_api::PluginContext & context,
  const YAML::Node & config)
{
  const auto & identity = specification.identity;
  const auto & requested = specification.descriptor;
  if (!valid_identity_segment(identity.instance_id)) {
    return Status::failure("插件实例标识不合法: " + identity.instance_id);
  }
  if (!valid_identity_segment(identity.device_id)) {
    return Status::failure("设备标识不合法: " + identity.device_id);
  }
  if (!valid_identity_segment(identity.component_id)) {
    return Status::failure("组件标识不合法: " + identity.component_id);
  }
  if (requested.class_type.empty()) {
    return Status::failure("插件 class_type 不能为空: " + identity.instance_id);
  }
  if (!specification.enabled) {
    return Status::failure("插件实例已在配置中禁用: " + identity.instance_id);
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (entries_.find(identity.instance_id) != entries_.end()) {
      return Status::failure("插件实例标识已存在: " + identity.instance_id);
    }
    Entry placeholder;
    placeholder.specification = specification;
    placeholder.transitioning = true;
    entries_.emplace(identity.instance_id, std::move(placeholder));
  }

  std::shared_ptr<uran_plugin_api::PluginContext> plugin_context;
  std::shared_ptr<uran_plugin_api::Plugin> instance;
  try {
    // pluginlib 类加载器的内部状态不作为并发契约，创建过程显式串行化。
    std::lock_guard<std::mutex> loader_lock(loader_mutex_);
    instance = loader_.createSharedInstance(requested.class_type);
  } catch (const pluginlib::PluginlibException & exception) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(identity.instance_id);
    return Status::failure(
      "加载插件类失败: " + requested.class_type + "，原因: " + exception.what());
  } catch (const std::exception & exception) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(identity.instance_id);
    return Status::failure(
      "创建插件实例失败: " + requested.class_type + "，原因: " + exception.what());
  } catch (...) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(identity.instance_id);
    return Status::failure("创建插件实例时发生未知异常: " + requested.class_type);
  }

  if (!instance) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(identity.instance_id);
    return Status::failure("插件加载器返回空实例: " + requested.class_type);
  }

  PluginDescriptor actual;
  try {
    actual = instance->descriptor();
  } catch (const std::exception & exception) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(identity.instance_id);
    return Status::failure(
      "读取插件实现描述失败: " + identity.instance_id + "，原因: " + exception.what());
  } catch (...) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(identity.instance_id);
    return Status::failure("读取插件实现描述时发生未知异常: " + identity.instance_id);
  }

  if (actual.plugin_type.empty() || actual.api_version.empty() || actual.class_type.empty()) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(identity.instance_id);
    return Status::failure(
      "插件实现描述缺少类型、API 版本或实现类: " + identity.instance_id);
  }
  if (actual.class_type != requested.class_type) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(identity.instance_id);
    return Status::failure("插件实现类与配置不一致: " + identity.instance_id);
  }
  if (!requested.plugin_type.empty() && actual.plugin_type != requested.plugin_type) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(identity.instance_id);
    return Status::failure("插件类型与配置不一致: " + identity.instance_id);
  }
  if (!requested.api_version.empty() && actual.api_version != requested.api_version) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(identity.instance_id);
    return Status::failure("插件 API 版本不兼容: " + identity.instance_id);
  }
  if (!requested.plugin_version.empty() &&
    actual.plugin_version != requested.plugin_version)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(identity.instance_id);
    return Status::failure("插件实现版本与配置不一致: " + identity.instance_id);
  }

  try {
    const auto node = context.node();
    if (!node) {
      std::lock_guard<std::mutex> lock(mutex_);
      entries_.erase(identity.instance_id);
      return Status::failure(
        "配置插件时 Runtime 节点已经失效: " + identity.instance_id);
    }
    auto state_access = std::make_shared<ScopedStateAccess>(
      context.state_access(), identity.instance_id);
    const auto instance_namespace = specification.ros_namespace.empty() ?
      build_instance_namespace(context.instance_namespace(), identity) :
      specification.ros_namespace;
    plugin_context = std::make_shared<uran_plugin_api::PluginContext>(
      node, std::move(state_access), context.route_resolver(),
      identity, instance_namespace);
  } catch (const std::exception & exception) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(identity.instance_id);
    return Status::failure(
      "创建插件受控上下文失败: " + identity.instance_id +
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
    entries_.erase(identity.instance_id);
    return Status::failure(
      "配置插件实例失败: " + identity.instance_id + "，原因: " + configured.message);
  }

  bool cancelled = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto iterator = entries_.find(identity.instance_id);
    if (iterator == entries_.end()) {
      cancelled = true;
    } else {
      iterator->second.specification = specification;
      iterator->second.specification.descriptor = actual;
      iterator->second.context = std::move(plugin_context);
      iterator->second.instance = instance;
      iterator->second.state = PluginState::configured;
      iterator->second.transitioning = false;
      iterator->second.error.clear();
    }
  }
  if (cancelled) {
    try {
      static_cast<void>(instance->cleanup());
    } catch (...) {
    }
    return Status::failure("插件加载事务已被取消: " + identity.instance_id);
  }
  return Status::success("插件实例已配置: " + identity.instance_id);
}

Status PluginManager::activate(const std::string & instance_id)
{
  std::shared_ptr<uran_plugin_api::Plugin> instance;
  PluginIdentity identity;
  int priority = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto * entry = find_entry(instance_id);
    if (entry == nullptr) {
      return Status::failure("插件实例不存在: " + instance_id);
    }
    if (entry->transitioning) {
      return Status::failure("插件实例正在执行其他生命周期操作: " + instance_id);
    }
    if (entry->state != PluginState::configured && entry->state != PluginState::inactive) {
      return Status::failure("当前状态不允许激活插件实例: " + instance_id);
    }
    entry->transitioning = true;
    instance = entry->instance;
    identity = entry->specification.identity;
    priority = entry->specification.priority;
  }

  Status result;
  try {
    result = instance->activate();
  } catch (const std::exception & exception) {
    result = Status::failure(std::string("插件 activate 抛出异常: ") + exception.what());
  } catch (...) {
    result = Status::failure("插件 activate 抛出未知异常");
  }

  if (result.ok && capability_registry_) {
    try {
      const auto capabilities = instance->capabilities();
      result = capability_registry_->register_instance(
        identity, capabilities.features, priority);
    } catch (const std::exception & exception) {
      result = Status::failure(
        std::string("读取或注册插件能力失败: ") + exception.what());
    } catch (...) {
      result = Status::failure("读取或注册插件能力时发生未知异常");
    }
    if (!result.ok) {
      // 能力登记失败时插件不能进入可用状态，尽力恢复到停止状态。
      try {
        static_cast<void>(instance->deactivate());
      } catch (...) {
      }
      static_cast<void>(capability_registry_->unregister_instance(instance_id));
    }
  }

  std::lock_guard<std::mutex> lock(mutex_);
  auto * entry = find_entry(instance_id);
  if (entry == nullptr || entry->instance != instance) {
    return Status::failure("插件激活事务失效: " + instance_id);
  }
  entry->transitioning = false;
  entry->state = result.ok ? PluginState::active : PluginState::error;
  entry->error = result.ok ? std::string{} : result.message;
  return result;
}

Status PluginManager::deactivate(const std::string & instance_id)
{
  std::shared_ptr<uran_plugin_api::Plugin> instance;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto * entry = find_entry(instance_id);
    if (entry == nullptr) {
      return Status::failure("插件实例不存在: " + instance_id);
    }
    if (entry->transitioning) {
      return Status::failure("插件实例正在执行其他生命周期操作: " + instance_id);
    }
    if (entry->state == PluginState::inactive || entry->state == PluginState::configured) {
      return Status::success("插件实例已经停止: " + instance_id);
    }
    if (entry->state != PluginState::active && entry->state != PluginState::error) {
      return Status::failure("当前状态不允许停用插件实例: " + instance_id);
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
  if (capability_registry_) {
    static_cast<void>(capability_registry_->unregister_instance(instance_id));
  }

  std::lock_guard<std::mutex> lock(mutex_);
  auto * entry = find_entry(instance_id);
  if (entry == nullptr || entry->instance != instance) {
    return Status::failure("插件停用事务失效: " + instance_id);
  }
  entry->transitioning = false;
  entry->state = result.ok ? PluginState::inactive : PluginState::error;
  entry->error = result.ok ? std::string{} : result.message;
  return result;
}

Status PluginManager::unload(const std::string & instance_id)
{
  PluginState state;
  // 上下文局部副本必须比插件实例局部副本存活久。
  std::shared_ptr<uran_plugin_api::PluginContext> plugin_context;
  std::shared_ptr<uran_plugin_api::Plugin> instance;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto * entry = find_entry(instance_id);
    if (entry == nullptr) {
      return Status::failure("插件实例不存在: " + instance_id);
    }
    if (entry->transitioning) {
      return Status::failure("插件实例正在执行其他生命周期操作: " + instance_id);
    }
    if (!entry->instance) {
      return Status::failure("插件实例尚未完成创建: " + instance_id);
    }
    state = entry->state;
    plugin_context = entry->context;
    instance = entry->instance;
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
    if (capability_registry_) {
      static_cast<void>(capability_registry_->unregister_instance(instance_id));
    }
    if (!stopped.ok) {
      std::lock_guard<std::mutex> lock(mutex_);
      auto * entry = find_entry(instance_id);
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
  auto * entry = find_entry(instance_id);
  if (entry == nullptr || entry->instance != instance) {
    return Status::failure("插件清理事务失效: " + instance_id);
  }
  if (!cleaned.ok) {
    entry->transitioning = false;
    entry->state = PluginState::error;
    entry->error = cleaned.message;
    return cleaned;
  }
  if (capability_registry_) {
    static_cast<void>(capability_registry_->unregister_instance(instance_id));
  }
  entries_.erase(instance_id);
  return Status::success("插件实例已卸载: " + instance_id);
}

Status PluginManager::configure_and_load_all(
  const YAML::Node & plugins_node,
  const uran_plugin_api::PluginContext & context)
{
  YAML::Node list = plugins_node;
  std::string default_device_id;
  if (plugins_node && plugins_node.IsMap()) {
    if (plugins_node["default_device_id"]) {
      try {
        default_device_id = plugins_node["default_device_id"].as<std::string>();
      } catch (const YAML::Exception & exception) {
        return Status::failure(
          std::string("默认设备标识配置无效: ") + exception.what());
      }
    }
    list = plugins_node["plugins"];
  }
  if (!list || !list.IsSequence()) {
    return Status::failure("插件配置必须包含 plugins 序列");
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
    const auto parsed = parse_plugin_spec(
      item, default_device_id, value.specification);
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
        "读取插件私有配置失败: " + value.specification.identity.instance_id +
        "，原因: " + exception.what());
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
    const auto & instance_id = item.specification.identity.instance_id;
    loaded_ids.push_back(instance_id);

    if (item.auto_activate) {
      const auto activated = activate(instance_id);
      if (!activated.ok) {
        for (auto iterator = loaded_ids.rbegin(); iterator != loaded_ids.rend(); ++iterator) {
          static_cast<void>(unload(*iterator));
        }
        return activated;
      }
    }
  }
  return Status::success("插件实例配置加载完成");
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
    info.identity = item.specification.identity;
    info.descriptor = item.specification.descriptor;
    info.state = item.state;
    info.error = item.error;
    if (item.instance) {
      try {
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

std::vector<ActivePluginInstance> PluginManager::active_instances() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<ActivePluginInstance> result;
  result.reserve(entries_.size());
  for (const auto & item : entries_) {
    if (!item.second.transitioning &&
      item.second.state == PluginState::active && item.second.instance)
    {
      result.push_back(ActivePluginInstance{
        item.second.specification.identity,
        item.second.specification.descriptor,
        item.second.specification.priority,
        item.second.instance});
    }
  }
  return result;
}

bool PluginManager::has(const std::string & instance_id) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.find(instance_id) != entries_.end();
}

PluginManager::Entry * PluginManager::find_entry(const std::string & instance_id)
{
  const auto iterator = entries_.find(instance_id);
  return iterator == entries_.end() ? nullptr : &iterator->second;
}

const PluginManager::Entry * PluginManager::find_entry(
  const std::string & instance_id) const
{
  const auto iterator = entries_.find(instance_id);
  return iterator == entries_.end() ? nullptr : &iterator->second;
}

bool PluginManager::valid_identity_segment(const std::string & value)
{
  static const std::regex pattern(R"(^[A-Za-z][A-Za-z0-9_]*$)");
  return std::regex_match(value, pattern);
}

}
