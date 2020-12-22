// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <thread>

#include "iree/hal/cts/cts_test_base.h"
#include "iree/hal/testing/driver_registry.h"
#include "iree/testing/gtest.h"

namespace iree {
namespace hal {
namespace cts {

class SemaphoreTest : public CtsTestBase {};

// Tests that a semaphore that is unused properly cleans itself up.
TEST_P(SemaphoreTest, NoOp) {
  IREE_ASSERT_OK_AND_ASSIGN(auto semaphore, device_->CreateSemaphore(123u));
  IREE_ASSERT_OK_AND_ASSIGN(uint64_t value, semaphore->Query());
  EXPECT_EQ(123u, value);
}

// Tests that a semaphore will accept new values as it is signaled.
TEST_P(SemaphoreTest, NormalSignaling) {
  IREE_ASSERT_OK_AND_ASSIGN(auto semaphore, device_->CreateSemaphore(2u));
  EXPECT_EQ(2u, semaphore->Query().value());
  IREE_EXPECT_OK(semaphore->Signal(3u));
  EXPECT_EQ(3u, semaphore->Query().value());
  IREE_EXPECT_OK(semaphore->Signal(40u));
  EXPECT_EQ(40u, semaphore->Query().value());
}

// Note: Behavior is undefined when signaling with decreasing values, so we
// can't reliably test it across backends. Some backends may return errors,
// while others may accept the new, decreasing, values.

// Tests that a semaphore that has failed will remain in a failed state.
TEST_P(SemaphoreTest, Failure) {
  IREE_ASSERT_OK_AND_ASSIGN(auto semaphore, device_->CreateSemaphore(2u));
  // Signal to 3.
  IREE_EXPECT_OK(semaphore->Signal(3u));
  EXPECT_EQ(3u, semaphore->Query().value());

  // Fail now.
  semaphore->Fail(UnknownErrorBuilder(IREE_LOC));
  EXPECT_TRUE(IsUnknown(semaphore->Query().status()));

  // Signaling again is undefined behavior. Some backends may return a
  // sticky failure status while others may silently process new signal values.
}

// Tests waiting on no semaphores.
TEST_P(SemaphoreTest, EmptyWait) {
  IREE_EXPECT_OK(device_->WaitAllSemaphores({}, InfiniteFuture()));
}

// Tests waiting on a semaphore that has already been signaled.
TEST_P(SemaphoreTest, WaitAlreadySignaled) {
  IREE_ASSERT_OK_AND_ASSIGN(auto semaphore, device_->CreateSemaphore(2u));
  // Test both previous and current values.
  IREE_EXPECT_OK(
      device_->WaitAllSemaphores({{semaphore.get(), 1u}}, InfiniteFuture()));
  IREE_EXPECT_OK(
      device_->WaitAllSemaphores({{semaphore.get(), 2u}}, InfiniteFuture()));
}

// Tests waiting on a semaphore that has not been signaled.
TEST_P(SemaphoreTest, WaitUnsignaled) {
  IREE_ASSERT_OK_AND_ASSIGN(auto semaphore, device_->CreateSemaphore(2u));
  // NOTE: we don't actually block here because otherwise we'd lock up.
  // Result status is undefined - some backends may return DeadlineExceededError
  // while others may return success.
  device_->WaitAllSemaphores({{semaphore.get(), 3u}}, InfinitePast())
      .IgnoreError();
}

// Waiting on a failed semaphore is undefined behavior. Some backends may
// return UnknownError while others may succeed.

// Waiting all semaphores but not all are signaled.
TEST_P(SemaphoreTest, WaitAllButNotAllSignaled) {
  IREE_ASSERT_OK_AND_ASSIGN(auto a, device_->CreateSemaphore(0u));
  IREE_ASSERT_OK_AND_ASSIGN(auto b, device_->CreateSemaphore(1u));
  // NOTE: we don't actually block here because otherwise we'd lock up.
  // Result status is undefined - some backends may return DeadlineExceededError
  // while others may return success.
  device_->WaitAllSemaphores({{a.get(), 1u}, {b.get(), 1u}}, InfinitePast())
      .IgnoreError();
}

// Waiting all semaphores and all are signaled.
TEST_P(SemaphoreTest, WaitAllAndAllSignaled) {
  IREE_ASSERT_OK_AND_ASSIGN(auto a, device_->CreateSemaphore(1u));
  IREE_ASSERT_OK_AND_ASSIGN(auto b, device_->CreateSemaphore(1u));
  IREE_ASSERT_OK(device_->WaitAllSemaphores({{a.get(), 1u}, {b.get(), 1u}},
                                            InfiniteFuture()));
}

// Waiting any semaphore to signal.
TEST_P(SemaphoreTest, WaitAny) {
  // TODO: fix this.
  if (driver_->name() == "dylib" || driver_->name() == "vmla" ||
      driver_->name() == "vulkan") {
    GTEST_SKIP();
  }

  IREE_ASSERT_OK_AND_ASSIGN(auto a, device_->CreateSemaphore(0u));
  IREE_ASSERT_OK_AND_ASSIGN(auto b, device_->CreateSemaphore(1u));
  IREE_ASSERT_OK(device_->WaitAnySemaphore({{a.get(), 1u}, {b.get(), 1u}},
                                           InfiniteFuture()));
}

// Tests threading behavior by ping-ponging between the test main thread and
// a little thread.
TEST_P(SemaphoreTest, PingPong) {
  IREE_ASSERT_OK_AND_ASSIGN(auto a2b, device_->CreateSemaphore(0u));
  IREE_ASSERT_OK_AND_ASSIGN(auto b2a, device_->CreateSemaphore(0u));
  std::thread thread([&]() {
    // Should advance right past this because the value is already set.
    IREE_ASSERT_OK(
        device_->WaitAllSemaphores({{a2b.get(), 0u}}, InfiniteFuture()));
    IREE_ASSERT_OK(b2a->Signal(1u));
    // Jump ahead.
    IREE_ASSERT_OK(
        device_->WaitAllSemaphores({{a2b.get(), 4u}}, InfiniteFuture()));
  });
  IREE_ASSERT_OK(
      device_->WaitAllSemaphores({{b2a.get(), 1u}}, InfiniteFuture()));
  IREE_ASSERT_OK(a2b->Signal(4u));
  thread.join();
}

INSTANTIATE_TEST_SUITE_P(
    AllDrivers, SemaphoreTest,
    ::testing::ValuesIn(testing::EnumerateAvailableDrivers()),
    GenerateTestName());

}  // namespace cts
}  // namespace hal
}  // namespace iree