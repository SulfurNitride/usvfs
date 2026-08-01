#include "semaphore.h"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <thread>

using namespace std::chrono_literals;

TEST(RecursiveBenaphoreTest, RecursiveOwnerExcludesAnotherThread)
{
  RecursiveBenaphore mutex;
  std::promise<void> ownerAcquired;
  std::promise<void> releaseOnce;
  std::promise<void> releasedOnce;
  std::promise<void> releaseFinally;
  std::promise<void> waiterStarted;
  std::promise<void> waiterAcquired;
  BenaphoreWaitKind ownerFirstResult{};
  BenaphoreWaitKind ownerRecursiveResult{};
  BenaphoreWaitKind waiterResult{};

  auto ownerAcquiredFuture  = ownerAcquired.get_future();
  auto releaseOnceFuture    = releaseOnce.get_future();
  auto releasedOnceFuture   = releasedOnce.get_future();
  auto releaseFinallyFuture = releaseFinally.get_future();
  auto waiterStartedFuture  = waiterStarted.get_future();
  auto waiterAcquiredFuture = waiterAcquired.get_future();

  std::thread owner([&]() {
    ownerFirstResult     = mutex.wait();
    ownerRecursiveResult = mutex.wait();
    ownerAcquired.set_value();

    releaseOnceFuture.wait();
    mutex.signal();
    releasedOnce.set_value();

    releaseFinallyFuture.wait();
    mutex.signal();
  });

  ownerAcquiredFuture.wait();
  std::thread waiter([&]() {
    waiterStarted.set_value();
    waiterResult = mutex.wait();
    waiterAcquired.set_value();
    mutex.signal();
  });

  waiterStartedFuture.wait();
  EXPECT_EQ(waiterAcquiredFuture.wait_for(250ms), std::future_status::timeout);

  releaseOnce.set_value();
  releasedOnceFuture.wait();
  EXPECT_EQ(waiterAcquiredFuture.wait_for(250ms), std::future_status::timeout);

  releaseFinally.set_value();
  EXPECT_EQ(waiterAcquiredFuture.wait_for(2s), std::future_status::ready);

  owner.join();
  waiter.join();

  EXPECT_EQ(ownerFirstResult, BenaphoreWaitKind::Uncontended);
  EXPECT_EQ(ownerRecursiveResult, BenaphoreWaitKind::Recursive);
  EXPECT_EQ(waiterResult, BenaphoreWaitKind::Contended);
}
