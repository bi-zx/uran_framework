#include <cstddef>
#include <string>

#include <gtest/gtest.h>

#include "uran_core_framework/message_router.hpp"

namespace uran_core_framework
{

TEST(MessageRouter, RoutesByExactMessageType)
{
  MessageRouter router;
  std::string received;
  std::size_t registration_id = 0U;

  ASSERT_TRUE(router.register_handler(
      "move.command",
      [&received](const uran_plugin_api::DownlinkEnvelope & message) {
        received = message.payload;
      },
      registration_id).ok);
  ASSERT_NE(registration_id, 0U);

  EXPECT_TRUE(router.route(
      uran_plugin_api::DownlinkEnvelope{"move.command", "payload", "request-1"}).ok);
  EXPECT_EQ(received, "payload");
}

TEST(MessageRouter, RejectsDuplicateHandler)
{
  MessageRouter router;
  std::size_t first_id = 0U;
  std::size_t second_id = 0U;
  const auto handler = [](const uran_plugin_api::DownlinkEnvelope &) {};

  ASSERT_TRUE(router.register_handler("task.command", handler, first_id).ok);
  EXPECT_FALSE(router.register_handler("task.command", handler, second_id).ok);
  EXPECT_EQ(second_id, 0U);
}

TEST(MessageRouter, UsesUnhandledHandlerOutsideRegistry)
{
  MessageRouter router;
  std::string received_type;
  router.set_unhandled_handler(
    [&received_type](const uran_plugin_api::DownlinkEnvelope & message) {
      received_type = message.message_type;
    });

  EXPECT_TRUE(router.route(
      uran_plugin_api::DownlinkEnvelope{"custom.event", "{}", ""}).ok);
  EXPECT_EQ(received_type, "custom.event");
}

}
