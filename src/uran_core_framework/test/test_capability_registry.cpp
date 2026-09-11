#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "uran_core_framework/capability_registry.hpp"

namespace uran_core_framework
{

TEST(CapabilityRegistry, SupportsMultipleProvidersAndStablePriorityOrder)
{
  CapabilityRegistry registry;
  const PluginIdentity primary{"dog_motion_primary", "robot_001", "locomotion"};
  const PluginIdentity backup{"dog_motion_backup", "robot_001", "locomotion"};

  ASSERT_TRUE(registry.register_instance(
      primary, {"motion.velocity", "motion.posture"}, 100).ok);
  ASSERT_TRUE(registry.register_instance(
      backup, {"motion.velocity"}, 50).ok);

  const auto providers = registry.providers(
    "motion.velocity", "robot_001", "locomotion");
  ASSERT_EQ(providers.size(), 2U);
  EXPECT_EQ(providers[0].identity.instance_id, "dog_motion_primary");
  EXPECT_EQ(providers[1].identity.instance_id, "dog_motion_backup");
  EXPECT_EQ(registry.capability_count(), 2U);
  EXPECT_EQ(registry.provider_count(), 3U);
}

TEST(CapabilityRegistry, KeepsDevicesAndComponentsIsolated)
{
  CapabilityRegistry registry;
  ASSERT_TRUE(registry.register_instance(
      {"front_camera", "robot_001", "camera_front"},
      {"image.capture"}, 0).ok);
  ASSERT_TRUE(registry.register_instance(
      {"arm_camera", "robot_001", "manipulator"},
      {"image.capture"}, 0).ok);
  ASSERT_TRUE(registry.register_instance(
      {"other_camera", "robot_002", "camera_front"},
      {"image.capture"}, 0).ok);

  EXPECT_EQ(registry.providers("image.capture", "robot_001").size(), 2U);
  const auto front = registry.providers(
    "image.capture", "robot_001", "camera_front");
  ASSERT_EQ(front.size(), 1U);
  EXPECT_EQ(front.front().identity.instance_id, "front_camera");
}

TEST(CapabilityRegistry, RegistrationIsTransactionalAndUnregistersWholeInstance)
{
  CapabilityRegistry registry;
  const PluginIdentity identity{"arm_main", "robot_001", "manipulator"};

  EXPECT_FALSE(registry.register_instance(
      identity, {"arm.pose_control", "invalid-capability"}, 0).ok);
  EXPECT_EQ(registry.capability_count(), 0U);

  ASSERT_TRUE(registry.register_instance(
      identity, {"arm.pose_control", "arm.joint_control"}, 0).ok);
  EXPECT_FALSE(registry.register_instance(
      {"arm_main", "robot_002", "manipulator"}, {"arm.status"}, 0).ok);
  EXPECT_FALSE(registry.register_instance(
      identity, {"arm.pose_control"}, 0).ok);
  ASSERT_TRUE(registry.unregister_instance(identity.instance_id).ok);
  EXPECT_TRUE(registry.capabilities_for_instance(identity.instance_id).empty());
  EXPECT_EQ(registry.provider_count(), 0U);
}

}
