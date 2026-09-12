#include <atomic>
#include <future>
#include <gtest/gtest.h>
#include "uran_platform_native/native_platform.hpp"
#include "uran_core_framework/scoped_platform_services.hpp"

using namespace std::chrono_literals;
TEST(NativePlatform, SerialExecutionAndExceptionIsolation)
{
  uran_platform_native::NativePlatform platform;
  std::atomic<int> count{0};
  std::promise<void> done;
  for (int i = 0; i < 20; ++i) {platform.post([&] {EXPECT_TRUE(platform.on_executor_thread()); ++count;});}
  platform.post([] {throw std::runtime_error("expected");});
  platform.post([&] {done.set_value();});
  ASSERT_EQ(done.get_future().wait_for(2s), std::future_status::ready);
  EXPECT_EQ(count, 20);
}
TEST(NativePlatform, ScopeRevokesTimersAndQueuedCallbacks)
{
  auto platform = std::make_shared<uran_platform_native::NativePlatform>();
  auto scope = std::make_shared<uran_core_framework::ScopedPlatformServices>(platform, "test");
  auto retained = std::make_shared<int>(1);
  std::weak_ptr<int> weak = retained;
  scope->schedule(1h, true, [retained] {});
  retained.reset();
  EXPECT_FALSE(weak.expired());
  scope->revoke();
  EXPECT_TRUE(weak.expired());
  EXPECT_THROW(scope->post([] {}), std::runtime_error);
  scope->enable();
  std::promise<void> done;
  scope->post([&] {done.set_value();});
  ASSERT_EQ(done.get_future().wait_for(2s), std::future_status::ready);
}
TEST(NativePlatform, CancelWaitsForRunningCallbackAndReleasesClosure)
{
  uran_platform_native::NativePlatform platform;
  std::promise<void> began, release;
  auto released = release.get_future().share();
  auto handle = platform.post([&] {began.set_value(); released.wait();});
  ASSERT_EQ(began.get_future().wait_for(2s), std::future_status::ready);
  auto cancellation = std::async(std::launch::async, [&] {handle->cancel();});
  EXPECT_EQ(cancellation.wait_for(20ms), std::future_status::timeout);
  release.set_value();
  ASSERT_EQ(cancellation.wait_for(2s), std::future_status::ready);
}
