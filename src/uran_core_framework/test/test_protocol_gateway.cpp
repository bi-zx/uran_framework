#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "uran_core_framework/protocol_gateway.hpp"

namespace uran_core_framework
{

class FakeProtocol final : public uran_plugin_api::ProtocolPlugin
{
public:
  FakeProtocol(std::string id, const bool publish_success)
  : id_(std::move(id)), publish_success_(publish_success)
  {
  }

  uran_plugin_api::PluginDescriptor descriptor() const override
  {
    return {id_, "protocol", "0.1.0", "1.0", "test/FakeProtocol"};
  }

  uran_plugin_api::Status configure(
    const uran_plugin_api::PluginContext &, const YAML::Node &) override
  {
    return uran_plugin_api::Status::success();
  }

  uran_plugin_api::Status activate() override
  {
    return uran_plugin_api::Status::success();
  }

  uran_plugin_api::Status deactivate() override
  {
    return uran_plugin_api::Status::success();
  }

  uran_plugin_api::Status cleanup() override
  {
    return uran_plugin_api::Status::success();
  }

  uran_plugin_api::PluginHealth health() const override
  {
    return {};
  }

  uran_plugin_api::PluginCapabilities capabilities() const override
  {
    return {};
  }

  uran_plugin_api::Status connect() override
  {
    return uran_plugin_api::Status::success();
  }

  uran_plugin_api::Status disconnect() override
  {
    return uran_plugin_api::Status::success();
  }

  uran_plugin_api::Status publish(const uran_plugin_api::UplinkEnvelope & message) override
  {
    ++publish_count_;
    if (!publish_success_) {
      return uran_plugin_api::Status::failure("模拟发送失败");
    }
    if (callback_) {
      callback_({message.message_type, message.payload, message.correlation_id});
    }
    return uran_plugin_api::Status::success();
  }

  void set_downlink_callback(uran_plugin_api::DownlinkCallback callback) override
  {
    callback_ = std::move(callback);
  }

  std::size_t publish_count() const noexcept
  {
    return publish_count_;
  }

private:
  std::string id_;
  bool publish_success_{false};
  std::size_t publish_count_{0U};
  uran_plugin_api::DownlinkCallback callback_;
};

TEST(ProtocolGateway, FallsBackWhenPreferredProtocolFails)
{
  auto gateway = std::make_shared<ProtocolGateway>();
  auto failed = std::make_shared<FakeProtocol>("failed", false);
  auto working = std::make_shared<FakeProtocol>("working", true);

  ASSERT_TRUE(gateway->register_protocol("failed", failed).ok);
  ASSERT_TRUE(gateway->register_protocol("working", working).ok);
  ASSERT_TRUE(gateway->set_primary("working").ok);

  const auto result = gateway->publish(
    uran_plugin_api::UplinkEnvelope{"event", "{}", "request-1", false},
    "failed");

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(failed->publish_count(), 1U);
  EXPECT_EQ(working->publish_count(), 1U);
}

TEST(ProtocolGateway, DeliversProtocolDownlinkToRuntimeHandler)
{
  auto gateway = std::make_shared<ProtocolGateway>();
  auto protocol = std::make_shared<FakeProtocol>("loopback", true);
  std::string received;
  gateway->set_downlink_handler(
    [&received](const uran_plugin_api::DownlinkEnvelope & message) {
      received = message.payload;
    });

  ASSERT_TRUE(gateway->register_protocol("loopback", protocol).ok);
  ASSERT_TRUE(gateway->publish(
      uran_plugin_api::UplinkEnvelope{"event", "payload", "", false}).ok);
  EXPECT_EQ(received, "payload");
}

TEST(ProtocolGateway, RejectsMessageWithoutType)
{
  auto gateway = std::make_shared<ProtocolGateway>();
  auto protocol = std::make_shared<FakeProtocol>("loopback", true);

  ASSERT_TRUE(gateway->register_protocol("loopback", protocol).ok);
  EXPECT_FALSE(gateway->publish(
      uran_plugin_api::UplinkEnvelope{"", "payload", "", false}).ok);
  EXPECT_EQ(protocol->publish_count(), 0U);
}

}
