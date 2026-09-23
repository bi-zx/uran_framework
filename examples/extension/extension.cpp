#include "uran_plugin_api/message_plugin.hpp"
#include "uran_plugin_api/plugin_loader.hpp"

class ExampleExtension final : public uran_plugin_api::MessagePlugin
{
public:
  uran_plugin_api::PluginDescriptor descriptor() const override
  {return {"extension", "1.0.0", uran_plugin_api::kPluginApiVersion, "example/Extension"};}
  uran_plugin_api::Status configure(const uran_plugin_api::PluginContext & context, const YAML::Node &) override
  {
    state_ = context.state_access();
    owner_ = context.instance_id();
    field_ = "plugin." + owner_ + ".last_message";
    return state_->register_field({field_, "json", "", "", false});
  }
  uran_plugin_api::Status activate() override {active_ = true; return uran_plugin_api::Status::success();}
  uran_plugin_api::Status deactivate() override {active_ = false; return uran_plugin_api::Status::success();}
  uran_plugin_api::Status cleanup() override
  {return state_ ? state_->unregister_field(field_, owner_) : uran_plugin_api::Status::success();}
  uran_plugin_api::PluginHealth health() const override
  {
    uran_plugin_api::PluginHealth result;
    result.level = active_ ? uran_plugin_api::PluginHealth::Level::healthy : uran_plugin_api::PluginHealth::Level::stopped;
    return result;
  }
  uran_plugin_api::PluginCapabilities capabilities() const override
  {return {{"example.echo"}, {}, {field_}};}
  std::vector<std::string> message_types() const override {return {"example.echo"};}
  uran_plugin_api::Status handle(const uran_plugin_api::DownlinkEnvelope & message) override
  {return state_->set(field_, message.payload, owner_);}
private:
  std::shared_ptr<uran_plugin_api::StateAccess> state_;
  std::string owner_, field_;
  bool active_{false};
};

extern "C" URAN_PLUGIN_API_EXPORT const uran_plugin_api::NativePluginExport * uran_plugin_export_v3()
{
  static const uran_plugin_api::NativePluginExport exported{
    uran_plugin_api::kNativePluginAbiVersion, "example/Extension",
    []() -> uran_plugin_api::Plugin * {return new ExampleExtension;},
    [](uran_plugin_api::Plugin * plugin) noexcept {delete plugin;}};
  return &exported;
}
