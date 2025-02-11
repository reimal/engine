// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/fuchsia/flutter/flatland_connection.h"

#include <fuchsia/scenic/scheduling/cpp/fidl.h>
#include <fuchsia/ui/composition/cpp/fidl.h>
#include <lib/async-testing/test_loop.h>

#include <string>
#include <vector>

#include "flutter/fml/logging.h"
#include "flutter/fml/time/time_delta.h"
#include "flutter/fml/time/time_point.h"
#include "gtest/gtest.h"

#include "fakes/scenic/fake_flatland.h"

namespace flutter_runner::testing {

namespace {

std::string GetCurrentTestName() {
  return ::testing::UnitTest::GetInstance()->current_test_info()->name();
}

void AwaitVsyncChecked(FlatlandConnection& flatland_connection,
                       bool& condition_variable,
                       fml::TimeDelta expected_frame_delta) {
  flatland_connection.AwaitVsync(
      [&condition_variable,
       expected_frame_delta = std::move(expected_frame_delta)](
          fml::TimePoint frame_start, fml::TimePoint frame_end) {
        EXPECT_EQ(frame_end.ToEpochDelta() - frame_start.ToEpochDelta(),
                  expected_frame_delta);
        condition_variable = true;
      });
}

}  // namespace

class FlatlandConnectionTest : public ::testing::Test {
 protected:
  FlatlandConnectionTest()
      : session_subloop_(loop_.StartNewLoop()),
        flatland_handle_(
            fake_flatland_.Connect(session_subloop_->dispatcher())) {}
  ~FlatlandConnectionTest() override = default;

  async::TestLoop& loop() { return loop_; }

  FakeFlatland& fake_flatland() { return fake_flatland_; }

  fidl::InterfaceHandle<fuchsia::ui::composition::Flatland>
  TakeFlatlandHandle() {
    FML_CHECK(flatland_handle_.is_valid());
    return std::move(flatland_handle_);
  }

 private:
  async::TestLoop loop_;
  std::unique_ptr<async::LoopInterface> session_subloop_;

  FakeFlatland fake_flatland_;

  fidl::InterfaceHandle<fuchsia::ui::composition::Flatland> flatland_handle_;
};

TEST_F(FlatlandConnectionTest, Initialization) {
  // Create the FlatlandConnection but don't pump the loop.  No FIDL calls are
  // completed yet.
  const std::string debug_name = GetCurrentTestName();
  flutter_runner::FlatlandConnection flatland_connection(
      debug_name, TakeFlatlandHandle(), []() { FAIL(); },
      [](auto...) { FAIL(); }, 1, fml::TimeDelta::Zero());
  EXPECT_EQ(fake_flatland().debug_name(), "");

  // Simulate an AwaitVsync that comes immediately.
  bool await_vsync_fired = false;
  AwaitVsyncChecked(flatland_connection, await_vsync_fired,
                    kDefaultFlatlandPresentationInterval);
  EXPECT_TRUE(await_vsync_fired);

  // Ensure the debug name is set.
  loop().RunUntilIdle();
  EXPECT_EQ(fake_flatland().debug_name(), debug_name);
}

TEST_F(FlatlandConnectionTest, FlatlandDisconnect) {
  // Set up a callback which allows sensing of the error state.
  bool error_fired = false;
  fml::closure on_session_error = [&error_fired]() { error_fired = true; };

  // Create the FlatlandConnection but don't pump the loop.  No FIDL calls are
  // completed yet.
  flutter_runner::FlatlandConnection flatland_connection(
      GetCurrentTestName(), TakeFlatlandHandle(), std::move(on_session_error),
      [](auto...) { FAIL(); }, 1, fml::TimeDelta::Zero());
  EXPECT_FALSE(error_fired);

  // Simulate a flatland disconnection, then Pump the loop.  The error callback
  // will fire.
  fake_flatland().Disconnect(
      fuchsia::ui::composition::FlatlandError::BAD_OPERATION);
  loop().RunUntilIdle();
  EXPECT_TRUE(error_fired);
}

TEST_F(FlatlandConnectionTest, BasicPresent) {
  // Set up callbacks which allow sensing of how many presents were handled.
  size_t presents_called = 0u;
  zx_handle_t release_fence_handle;
  fake_flatland().SetPresentHandler([&presents_called,
                                     &release_fence_handle](auto present_args) {
    presents_called++;
    release_fence_handle = present_args.release_fences().empty()
                               ? ZX_HANDLE_INVALID
                               : present_args.release_fences().front().get();
  });

  // Set up a callback which allows sensing of how many vsync's
  // (`OnFramePresented` events) were handled.
  size_t vsyncs_handled = 0u;
  on_frame_presented_event on_frame_presented = [&vsyncs_handled](auto...) {
    vsyncs_handled++;
  };

  // Create the FlatlandConnection but don't pump the loop.  No FIDL calls are
  // completed yet.
  flutter_runner::FlatlandConnection flatland_connection(
      GetCurrentTestName(), TakeFlatlandHandle(), []() { FAIL(); },
      std::move(on_frame_presented), 1, fml::TimeDelta::Zero());
  EXPECT_EQ(presents_called, 0u);
  EXPECT_EQ(vsyncs_handled, 0u);

  // Pump the loop. Nothing is called.
  loop().RunUntilIdle();
  EXPECT_EQ(presents_called, 0u);
  EXPECT_EQ(vsyncs_handled, 0u);

  // Simulate an AwaitVsync that comes after the first call.
  bool await_vsync_fired = false;
  AwaitVsyncChecked(flatland_connection, await_vsync_fired,
                    kDefaultFlatlandPresentationInterval);
  EXPECT_TRUE(await_vsync_fired);

  // Call Present and Pump the loop; `Present` and its callback is called. No
  // release fence should be queued.
  await_vsync_fired = false;
  zx::event first_release_fence;
  zx::event::create(0, &first_release_fence);
  const zx_handle_t first_release_fence_handle = first_release_fence.get();
  flatland_connection.EnqueueReleaseFence(std::move(first_release_fence));
  flatland_connection.Present();
  loop().RunUntilIdle();
  EXPECT_EQ(presents_called, 1u);
  EXPECT_EQ(release_fence_handle, ZX_HANDLE_INVALID);
  EXPECT_EQ(vsyncs_handled, 0u);
  EXPECT_FALSE(await_vsync_fired);

  // Fire the `OnNextFrameBegin` event. AwaitVsync should be fired.
  AwaitVsyncChecked(flatland_connection, await_vsync_fired,
                    kDefaultFlatlandPresentationInterval);
  fuchsia::ui::composition::OnNextFrameBeginValues on_next_frame_begin_values;
  on_next_frame_begin_values.set_additional_present_credits(3);
  fake_flatland().FireOnNextFrameBeginEvent(
      std::move(on_next_frame_begin_values));
  loop().RunUntilIdle();
  EXPECT_TRUE(await_vsync_fired);

  // Fire the `OnFramePresented` event associated with the first `Present`,
  fake_flatland().FireOnFramePresentedEvent(
      fuchsia::scenic::scheduling::FramePresentedInfo());
  loop().RunUntilIdle();
  EXPECT_EQ(vsyncs_handled, 1u);

  // Call Present for a second time and Pump the loop; `Present` and its
  // callback is called. Release fences for the earlier present is used.
  await_vsync_fired = false;
  flatland_connection.Present();
  loop().RunUntilIdle();
  EXPECT_EQ(presents_called, 2u);
  EXPECT_EQ(release_fence_handle, first_release_fence_handle);
}

}  // namespace flutter_runner::testing
