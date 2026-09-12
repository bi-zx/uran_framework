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
  explicit FakeProtocol(const bool publish_success)
  : publish_success_(publish_success)
  {
  }

  uran_plugin_api::PluginDescriptor descriptor() const override
  {
    return {"protocol", "0.1.0", "1.0", "test/FakeProtocol"};
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

  uran_plugin_api::DownlinkCallback callback() const {return callback_;}

private:
  bool publish_success_{false};
  std::size_t publish_count_{0U};
  uran_plugin_api::DownlinkCallback callback_;
};

TEST(ProtocolGateway, FallsBackWhenPreferredProtocolFails)
{
  auto gateway = std::make_shared<ProtocolGateway>();
  auto failed = std::make_shared<FakeProtocol>(false);
  auto working = std::make_shared<FakeProtocol>(true);

  ASSERT_TRUE(gateway->register_protocol(
      {"failed", "robot_001", "communication"}, failed, 10).ok);
  ASSERT_TRUE(gateway->register_protocol(
      {"working", "robot_001", "communication"}, working, 100).ok);
  ASSERT_TRUE(gateway->set_primary_instance("working").ok);

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
  auto protocol = std::make_shared<FakeProtocol>(true);
  std::string received;
  std::string source_instance;
  gateway->set_downlink_handler(
    [&received, &source_instance](
      const PluginIdentity & identity,
      const uran_plugin_api::DownlinkEnvelope & message)
    {
      received = message.payload;
      source_instance = identity.instance_id;
    });

  ASSERT_TRUE(gateway->register_protocol(
      {"loopback", "robot_001", "communication"}, protocol).ok);
  ASSERT_TRUE(gateway->publish(
      uran_plugin_api::UplinkEnvelope{"event", "payload", "", false}).ok);
  EXPECT_EQ(received, "payload");
  EXPECT_EQ(source_instance, "loopback");
}

TEST(ProtocolGateway, RejectsMessageWithoutType)
{
  auto gateway = std::make_shared<ProtocolGateway>();
  auto protocol = std::make_shared<FakeProtocol>(true);

  ASSERT_TRUE(gateway->register_protocol(
      {"loopback", "robot_001", "communication"}, protocol).ok);
  EXPECT_FALSE(gateway->publish(
      uran_plugin_api::UplinkEnvelope{"", "payload", "", false}).ok);
  EXPECT_EQ(protocol->publish_count(), 0U);
}

TEST(ProtocolGateway, TreatsSameImplementationAsIndependentInstances)
{
  auto gateway = std::make_shared<ProtocolGateway>();
  auto primary = std::make_shared<FakeProtocol>(true);
  auto backup = std::make_shared<FakeProtocol>(true);

  ASSERT_TRUE(gateway->register_protocol(
      {"protocol_primary", "robot_001", "communication"}, primary, 100).ok);
  ASSERT_TRUE(gateway->register_protocol(
      {"protocol_backup", "robot_001", "communication"}, backup, 50).ok);

  EXPECT_EQ(gateway->protocol_instance_ids().size(), 2U);
  EXPECT_EQ(gateway->primary_instance_id(), "protocol_primary");
  ASSERT_TRUE(gateway->publish(
      uran_plugin_api::UplinkEnvelope{"event", "{}", "", false},
      "protocol_backup").ok);
  EXPECT_EQ(primary->publish_count(), 0U);
  EXPECT_EQ(backup->publish_count(), 1U);
}

TEST(ProtocolGateway, ScopedUplinkCannotFallBackToAnotherDevice)
{
  auto gateway = std::make_shared<ProtocolGateway>();
  auto local = std::make_shared<FakeProtocol>(false);
  auto other = std::make_shared<FakeProtocol>(true);
  ASSERT_TRUE(gateway->register_protocol({"local", "robot_a", "protocol"}, local, 10).ok);
  ASSERT_TRUE(gateway->register_protocol({"other", "robot_b", "protocol"}, other, 100).ok);
  uran_plugin_api::UplinkEnvelope message{"event", "{}", "", false};
  message.source = {"extension", "robot_a", "extension"};
  EXPECT_FALSE(gateway->publish(message, "other").ok);
  EXPECT_EQ(local->publish_count(), 1U);
  EXPECT_EQ(other->publish_count(), 0U);
}

TEST(ProtocolGateway, PreviousGenerationCannotInjectAfterReactivation)
{
  auto gateway = std::make_shared<ProtocolGateway>();
  auto protocol = std::make_shared<FakeProtocol>(true);
  const PluginIdentity identity{"protocol", "robot_a", "communication"};
  ASSERT_TRUE(gateway->register_protocol(identity, protocol).ok);
  const auto obsolete = protocol->callback();
  ASSERT_TRUE(gateway->unregister_protocol(identity.instance_id).ok);
  ASSERT_TRUE(gateway->register_protocol(identity, protocol).ok);
  int received = 0;
  gateway->set_downlink_handler([&](const auto &, const auto & message) {
    ++received;
    EXPECT_EQ(message.source, identity);
    EXPECT_TRUE(gateway->current_source(message.source, message.source_generation));
  });
  uran_plugin_api::DownlinkEnvelope forged{"event", "{}", ""};
  forged.source = {"forged", "robot_b", "other"};
  obsolete(forged);
  EXPECT_EQ(received, 0);
  protocol->callback()(forged);
  EXPECT_EQ(received, 1);
}

}
