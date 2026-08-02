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

TEST(RecursiveSharedMutexTest, ReadersOverlapAndWriterWaits)
{
  RecursiveSharedMutex mutex;
  std::promise<void> firstReaderReady;
  std::promise<void> releaseFirstReader;
  std::promise<void> secondReaderReady;
  std::promise<void> releaseSecondReader;
  std::promise<void> writerStarted;
  std::promise<void> writerReady;

  auto firstReaderReadyFuture    = firstReaderReady.get_future();
  auto releaseFirstReaderFuture  = releaseFirstReader.get_future();
  auto secondReaderReadyFuture   = secondReaderReady.get_future();
  auto releaseSecondReaderFuture = releaseSecondReader.get_future();
  auto writerStartedFuture       = writerStarted.get_future();
  auto writerReadyFuture         = writerReady.get_future();

  std::thread firstReader([&]() {
    EXPECT_EQ(mutex.lockShared(), BenaphoreWaitKind::Uncontended);
    firstReaderReady.set_value();
    releaseFirstReaderFuture.wait();
    mutex.unlockShared();
  });
  firstReaderReadyFuture.wait();

  std::thread secondReader([&]() {
    EXPECT_EQ(mutex.lockShared(), BenaphoreWaitKind::Uncontended);
    secondReaderReady.set_value();
    releaseSecondReaderFuture.wait();
    mutex.unlockShared();
  });
  EXPECT_EQ(secondReaderReadyFuture.wait_for(2s), std::future_status::ready);

  std::thread writer([&]() {
    writerStarted.set_value();
    EXPECT_EQ(mutex.lockExclusive(), BenaphoreWaitKind::Contended);
    writerReady.set_value();
    mutex.unlockExclusive();
  });
  writerStartedFuture.wait();
  EXPECT_EQ(writerReadyFuture.wait_for(250ms), std::future_status::timeout);

  releaseFirstReader.set_value();
  firstReader.join();
  EXPECT_EQ(writerReadyFuture.wait_for(250ms), std::future_status::timeout);

  releaseSecondReader.set_value();
  secondReader.join();
  EXPECT_EQ(writerReadyFuture.wait_for(2s), std::future_status::ready);
  writer.join();
}

TEST(RecursiveSharedMutexTest, SupportsReadAndWriteRecursionWithoutUpgrade)
{
  RecursiveSharedMutex mutex;

  EXPECT_FALSE(mutex.heldByCurrentThread());
  EXPECT_EQ(mutex.lockShared(), BenaphoreWaitKind::Uncontended);
  EXPECT_TRUE(mutex.heldByCurrentThread());
  EXPECT_EQ(mutex.lockShared(), BenaphoreWaitKind::Recursive);
  EXPECT_THROW(mutex.lockExclusive(), std::logic_error);
  EXPECT_FALSE(mutex.unlockShared());
  EXPECT_TRUE(mutex.unlockShared());
  EXPECT_FALSE(mutex.heldByCurrentThread());

  EXPECT_EQ(mutex.lockExclusive(), BenaphoreWaitKind::Uncontended);
  EXPECT_EQ(mutex.lockExclusive(), BenaphoreWaitKind::Recursive);
  EXPECT_EQ(mutex.lockShared(), BenaphoreWaitKind::Recursive);
  EXPECT_FALSE(mutex.unlockShared());
  EXPECT_FALSE(mutex.unlockExclusive());
  EXPECT_TRUE(mutex.unlockExclusive());
  EXPECT_FALSE(mutex.heldByCurrentThread());
}
