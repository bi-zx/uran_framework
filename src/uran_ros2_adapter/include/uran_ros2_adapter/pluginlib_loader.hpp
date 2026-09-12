#pragma once
#include <pluginlib/class_loader.hpp>
#include "uran_plugin_api/plugin_loader.hpp"

namespace uran_ros2_adapter
{
class PluginlibLoader final : public uran_plugin_api::PluginLoader
{
public:
  PluginlibLoader()
  : loader_(std::make_shared<pluginlib::ClassLoader<uran_plugin_api::Plugin>>(
      "uran_ros2_adapter", "uran_plugin_api::Plugin")) {}
  std::vector<std::string> available_classes() const override
  {return loader_->getDeclaredClasses();}
  std::shared_ptr<uran_plugin_api::Plugin> create(const std::string & name) override
  {
    struct Owner {
      std::shared_ptr<pluginlib::ClassLoader<uran_plugin_api::Plugin>> loader;
      std::shared_ptr<uran_plugin_api::Plugin> plugin;
    };
    auto owner = std::make_shared<Owner>();
    owner->loader = loader_;
    owner->plugin = loader_->createSharedInstance(name);
    return std::shared_ptr<uran_plugin_api::Plugin>(owner, owner->plugin.get());
  }
private:
  std::shared_ptr<pluginlib::ClassLoader<uran_plugin_api::Plugin>> loader_;
};
}
